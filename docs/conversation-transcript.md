# Development transcript — `mckeylog` (MeshCentral keylogger)

A condensed, technical transcript of the sessions that built this project,
written so another AI (or a human) can pick the work up without the original
conversations. The original chat was long and iterative; this keeps the
decisions, the mistakes, and the gotchas.

**Context**: non-technical Windows user, casual tone, at the keyboard. Server:
a local MeshCentral install on the same machine as the agent (localhost),
`C:\Program Files\Open Source\MeshCentral`, Node 26, agent installed at
`C:\Program Files\Mesh Agent`. Pattern reference: the MeshCentral **Audio**
project ("mic-central") — native C module in MeshAgent + plugin with a
first-class device-page tab.

---

## Phase 1 — design

Goal: a keylogger for MeshCentral, mic-central-style:

- native C (no third-party deps), compiled into MeshAgent;
- first-class **Keylogs** tab on device pages (not a generic plugin panel):
  plugin `mckeylog`, **tunnel protocol 17**, **page ID 61** / panel `p61`;
- live keystroke view + browse/download of rolling local log files;
- keylogging runs **24/7** — including when the agent is a session-0 service.

Chosen plumbing (mirrors audio/protocol 15):

- `meshrelay.js`: allow protocol 17 only with the desktop rights (admin or
  remote-control/view without no-desktop) — `isProtocolAllowedByRights`.
- `agents/meshcore.js`: tunnel dispatch branch for protocol 17,
  `descriptorMetadata = "Remote Keylogs"`, forwards to
  `require("win-keylogger").handleTunnelData(this, data)`; auto-start
  `require('keylogger').start({})` on win32.
- `views/default.handlebars` + `views/default3.handlebars`: script include,
  top-bar **Keylogs** tab, panel map `61: 'MainDevKeylogs'`, loop bounds,
  `p61` markup, `go()` lifecycle hooks.
- Agent side: `modules_meshcore/win-keylogger.js` — `tail` (250 ms drain,
  backpressure on `ws.write === false`), `start`, `stop`, `state`, `list`,
  `read` (64 KB base64 chunks), `unsub`.

## Phase 2 — first native implementation

`agent/ILibDuktape_Keylogger.{c,h}`, following the Audio module's conventions:

- **Process-wide singleton** `g_kl` (deliberately *not* per Duktape instance —
  the WASAPI module's per-`this` design is fragile across JS engine restarts).
- `WH_KEYBOARD_LL` on a dedicated thread with a `PeekMessageW` loop; thread
  parks in `WaitForSingleObject(stopEvent, 50)` so stop is fast (LL hooks time
  out at ~300 ms). Auto-repeat (`LLKHF_REPEAT`) skipped.
- Character resolution: `ToUnicodeEx` with synthesized key state from
  `GetAsyncKeyState`/`GetKeyState`.
- Output: 4096×384 in-memory ring + rolling `keylog-YYYY-MM-DD.log` files
  (10 MB default, `-1`/`-2` rotation, 1–256 MB configurable) next to the agent
  exe. File names strictly validated (`keylog-YYYY-MM-DD[-N].log`) before path
  use.
- Non-Windows stub so `require('keylogger')` behaves the same everywhere.

Build: `scripts/patch_meshagent.js` (idempotent) copies the C module in,
patches `ILibDuktape_ScriptContainer.c` (include + init under `#ifdef WIN32`),
vcxproj ClCompile/ClInclude items for all meshservice/meshconsole projects,
makefile entries.

**Build gotcha**: the solution's x86 platform is named `x86`, not `Win32` —
`/p:Platform=Win32` fails with MSB4126. x64 builds to a separate
`/p:OutDir` (incremented per build: `x64buildN`).

## Phase 3 — Node 26 server problems (found in production)

The user's server runs **Node 26**, which changed readable-stream internals.
Two real failures showed up:

1. **yauzl hang** — MeshCentral's plugin-zip extraction (`node_modules/fd-slicer`,
   used by yauzl) hung: on Node ≥ 26, `stream.destroyed` is backed by internal
   stream state, and fd-slicer's EOF path set `self.destroyed = true` *before*
   `self.push(null)`. Once destroyed, later `resume()`/`pause()` are no-ops, so
   the final partial chunk never drained and `end` never fired → the plugin
   install UI hung forever. Fix: `push(null)` before the destroyed-flag work
   (both the `toRead <= 0` and `bytesRead === 0` paths).
2. **Noisy post-end destroy** — Node 26 auto-destroys readable streams right
   after `end`; fd-slicer's `destroy()` then emitted a bogus `error`. Fix: when
   `!err && _readableState.ended`, treat the destroy as normal lifecycle —
   stay silent, skip the unref.
3. **Last zip entry truncated** — `pluginHandler.js` advanced to the next zip
   entry on the *source* stream's `end`, which fires before the write
   stream's buffer flushes; the last file of a plugin zip could come out
   empty. Fix: pipe into an explicit `ws` and advance on
   `ws.on('finish')`.

These were first fixed ad hoc, then ported into `install-keylogger.ps1` as
`Apply-TextPatch` / `Invoke-NodeCompatPatches`: exact-match text patches,
idempotent (marker string), `.bak-mckeylog` backups, clear "skipped — pattern
not found" warning if a MeshCentral upgrade changed the code. **They must be
re-applied after any MeshCentral server upgrade.**

## Phase 4 — the session-0 problem (the big one)

The keylogger "worked" but captured nothing when the agent ran as a **service
(session 0)**: from session 0, `OpenInputDesktop` cannot reach the console
session's input desktop, so a `WH_KEYBOARD_LL` hook installed there never sees
the user's keys. (Window-title fallback via `EnumWindows` worked — that's how
it was diagnosed: correct window titles, zero keys.)

Fix: **a user-session worker process**.

- The agent (session 0) relaunches **itself** as `"<exe>" keylog-worker <0|1>`
  via `CreateProcessAsUserW` on a duplicated console-user primary token:
  `WTSGetActiveConsoleSessionId` → `WTSQueryUserToken` → `DuplicateTokenEx`
  (TokenPrimary) → `SetTokenInformation(TokenSessionId)`.
- `ServiceMain.c` gets a branch: if `argv[1] == "keylog-worker"`, call
  `ILibDuktape_KeyloggerWorker_Run(argc, argv)` instead of normal startup.
  One binary, two roles.
- The worker lives in the interactive session, installs the same LL hook
  (naturally on that session's desktop) and streams formatted lines back to
  the agent over a **named pipe** `\\.\pipe\mckeylog` (fixed 400-byte
  records).
- Pipe subtlety: a pipe created by a SYSTEM service is **not writable** from a
  medium-integrity user session by default (client gets error 5). The agent
  creates the pipe with an explicit ACE for `Everyone`
  (`D:(A;;GRGW;;;WD)(A;;GA;;;SY)(A;;GA;;;BA)`).
- Record layout: **2-byte little-endian key count (0 for window markers) +
  line text** (NUL-padded). The key count exists so the agent's `totalKeys`
  counts real keystrokes, not formatted lines.
- The worker dies on logoff/user switch; the agent's thread detects the
  exit (waitable process handle + `ERROR_BROKEN_PIPE` on the pipe) and
  re-spawns with backoff (`KEYLOG_SPAWN_RETRY_MS`). Capture is 24/7 across
  sessions, with the agent process itself never restarting.
- If the agent runs *in* a user session (console agent, not a service), the
  in-process hook captures directly and no worker is spawned.

**Deploy gotcha**: an orphaned session-1 `keylog-worker` (or old
`MeshAgent.exe`) from a previous agent instance **survives stopping the
service and locks the exe** during replacement. Always `taskkill` every
`MeshAgent*` process before swapping the binaries.

## Phase 5 — NanoCore-style readable output

First version logged one line per keypress
(`YYYY-MM-DD HH:MM:SS.mmm  <char>  <mods>  <window>`) — unusable for reading
"what was typed". Replaced with a **stateful formatter** in the worker (and in
the in-process hook for user-session agents), per process, lock-free by
design (LL hook callbacks are delivered on the installing thread, which is the
same thread as the pump loop — no cross-thread access):

- **Current window** tracked; on change → flush pending text, then emit a
  `Window: <title>` marker line.
- **~320-char UTF-8 text buffer**: typed chars appended; **Backspace strips
  the last UTF-8 character** (multi-byte aware); flushed as a line on
  **Enter**, when the buffer is near full, or after **≥ 1500 ms idle** (a
  250 ms `Keylog_FormatTick` in both the worker loop and the agent hook loop).
- **Special keys inline**: `[Enter]`, `[Tab]`, `[F5]`, …; **Ctrl+letter →
  `[Ctrl+X]`**; bare modifier down/ups are not logged.
- **Line format**: `YYYY-MM-DD HH:MM:SS  -  <body>` — the date prefix is kept
  on purpose: log files stay grep-friendly and the file-date rollover logic
  derives from it.

Browser side (`plugin/mckeylog-viewmode.js`): `lineHtml()` renders entries
(one entry can be 2 lines: marker + text), `WINDOW_LINE_RE` matches
`Window:` lines, `ensureLiveStyle()` injects
`#p61live .mckl-win{color:#4fc3f7;font-weight:bold}` once — window markers
bold blue, rest plain text. `renderLive` uses innerHTML and splits
multi-line entries.

Bugs found and fixed along the way:

- **`Keylog_Read` duplicate/loss bug**: the ring read used a stale base index;
  the index must advance **per iteration**:
  `(g_kl.ringHead + i) % KEYLOG_RING_LINES`.
- Old log files written by the pre-fix build have mixed NUL/LF line endings
  until a date rollover — acceptable, not worth migrating.

**Verified end-to-end** (sendkeys + real typing):

```
2026-11-11 05:39:00  -  keylogger e2e test 123
2026-11-11 17:05:43  -  Window: Connect GitHub PAT to opencode · OpenCode
2026-11-11 17:05:44  -  ]u
2026-11-11 17:05:45  -  upload to it all to the github project alon4gs4
```

(`]u` = `alt`+`u` on a German keyboard, and the typo is the user's — the
point is it reads like a transcript.)

## Phase 6 — deployment, self-update loop, verification

- Server restarts wipe the in-memory browser session (re-login needed) and
  trigger the **agent self-update dance**: the server hashes the serving agent
  binary + random MSH per startup (`updateMeshAgentsTable` in
  `meshcentral.js`); the agent hashes its own exe
  (`GenerateSHA384FileHash(agentHost->exePath, agentHash)`, `agentcore.c`);
  mismatch → agent downloads, replaces, restarts. Convergence takes **~5–6
  cycles (1–2 min)** after any server restart — normal, do not "fix" it.
- `keylog-debug.log` (next to the agent exe) is written by the agent
  (SYSTEM) and needed for diagnosis; ACL `Everyone:F` lets a user session
  read it. The debug file doubles as the worker/pipe diagnostic
  (`worker: spawned pid=...`, `worker: hook installed`,
  `WTSQueryUserToken FAILED ...`).
- Final verified state: agent service (session 0) + `keylog-worker`
  (session 1) spawned at logon/auto-start, files on disk, live view readable
  with blue window markers, no duplicates.

## Phase 7 — this session (repo sync + publish)

- Synced the repo with the deployed truth: `agent/ILibDuktape_Keylogger.{c,h}`
  (formatter + worker), new x64/x86 binaries, `win-keylogger.js` (auto-start
  at load), `mckeylog-viewmode.js` (marker styling), `patch_meshagent.js`
  (added the `ServiceMain.c` `keylog-worker` patch),
  `install-keylogger.ps1` (ported the three Node 26 compat patches in with
  `Apply-TextPatch`/`Invoke-NodeCompatPatches`).
- Verified the compat-patch text patterns against the real pre-patch backups
  (fd-slicer: patched-a-copy is **byte-identical** to the deployed file;
  pluginHandler: round-trip identity).
- Docs: README rewritten with a full "install & set up on your own
  MeshCentral server" walkthrough; `docs/architecture.md` rewritten for the
  worker design + output format; this transcript added.
- Version bumped 0.1.0 → 0.2.0.

## Operating notes / gotchas (read before touching anything)

1. **Deploy order**: kill *all* `MeshAgent*` processes (orphaned session-1
   workers lock the exe) → replace `C:\Program Files\Mesh Agent\MeshService64.exe`
   **and** the serving copy `meshcentral-data\agents\MeshService64.exe` (they
   must match or the self-update dance runs) → start service → fix debug-log
   ACL → restart MeshCentral server (triggers the ~5–6 cycle convergence).
2. **x86 build**: `/p:Platform=x86` (not `Win32`, MSB4126).
3. **Node 26 compat patches** are idempotent but must be re-run after a
   MeshCentral upgrade (they patch `node_modules`).
4. The formatter is intentionally **per process** and lock-free: the LL hook
   callback thread *is* the pump thread. Don't "clean this up" into
   cross-thread shared state without moving the formatting too.
5. Pipe records are a **fixed 400 bytes** (`KEYLOG_PIPE_RECORD`); changing the
   layout means changing both worker and agent sides (and the key count is
   what feeds `totalKeys`).
6. Idle flush is 1500 ms / tick 250 ms — the live view can lag real-time by up
   to ~1.5 s for the final words of a sentence; that's the flush design, not
   a bug.
7. Every server restart: browser must re-login (in-memory sessions) and every
   agent runs its update dance. Expected.
8. PowerShell 5.1: chain with `;` not `&&`; `$pid` is read-only; single-quoted
   strings need `''` for an embedded quote (this bit the installer once).

## Open items / known quirks

- Pre-rollover old log files may have NUL-terminated lines (mixed endings).
- The keylogger sees injected keys (standard LL-hook behavior) and only
  key-downs (no key-up semantics).
- Live web view was last confirmed by the user after a re-login (blue
  markers, readable lines, no duplicates).
- Scheduled task `mckeylog-s0test` was a temporary session-0 test task —
  remove it: `schtasks /Delete /F /TN mckeylog-s0test` (elevated).

## Where everything lives

| Thing | Location |
| --- | --- |
| This repo (source of truth) | `meshcentral-keylogger/` (GitHub `og-debug447/meshcentral-keylogger`) |
| Live MeshAgent checkout (where builds happen) | `meshcentral keylogger\MeshAgent\` (patched, builds to `x64buildN\`) |
| Installed agent | `C:\Program Files\Mesh Agent\` (+ `keylog-*.log`, `keylog-debug.log`) |
| MeshCentral server | `C:\Program Files\Open Source\MeshCentral\` (+ `meshcentral-data\`) |
| Deployed plugin JS | `meshcentral-data\plugins\mckeylog\` and `node_modules\meshcentral\public\scripts\mckeylog-viewmode.js` |
| Ad hoc deploy/test scripts | `C:\Users\Laptop\AppData\Local\Temp\opencode\` (`deploy-nanocore-3.ps1`, `run-elevated.ps1`, `sendkeys.ps1`, `fix-acl.ps1`, `patch-fdslicer*.ps1`, `patch-pluginhandler.ps1`) |
