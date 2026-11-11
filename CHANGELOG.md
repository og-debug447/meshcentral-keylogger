# Changelog

## 0.2.0 — 2026-11-11

- **NanoCore-style readable output**: the live pane and log files now show
  assembled text lines with `Window:` markers
  (`2026-11-11 17:05:47  -  hello world this is a test`) instead of one line
  per keypress. Backspace strips the last character; special keys render
  inline (`[Enter]`, `[Ctrl+X]`, `[F5]`); idle typing (≥ 1.5 s) auto-flushes.
  Window markers render blue in the tab; no duplicate lines.
- **24/7 capture from a session-0 service**: the agent now relaunches itself
  as a hidden `keylog-worker` in the interactive user's session
  (`CreateProcessAsUser` + console-user token) which captures the console
  user's keystrokes and streams them over `\\.\pipe\mckeylog` (fixed 400-byte
  records: 2-byte key count + line). The agent re-spawns the worker on
  logoff / user switch. `ServiceMain.c` gets a `keylog-worker` branch (added
  to `patch_meshagent.js`).
- **`totalKeys` now counts real keystrokes** (key counts summed from pipe
  records), not formatted lines; the ring read index bug (`Keylog_Read`) is
  fixed so `read()` returns lines in order.
- **Auto-start**: `win-keylogger.js` starts the keylogger at agent load, so
  nothing else is needed after an agent update/reinstall.
- **Node.js ≥ 26 compatibility**: `install-keylogger.ps1` now applies
  idempotent `.bak-mckeylog`-backed patches to `node_modules/fd-slicer` (yauzl
  hang on final partial chunk + noisy post-`end` auto-destroy) and
  `pluginHandler.js` (last zip entry truncated; advance on write-stream
  `finish`). Re-apply after any MeshCentral upgrade.
- **Docs**: full install/setup walkthrough for your own MeshCentral server in
  the README; `docs/architecture.md` rewritten for the worker design;
  `docs/conversation-transcript.md` — development transcript for other AIs.
- Fresh x64/x86 agent binaries in `binaries/` built from the updated source.

## 0.1.0 — 2026-11-11

- First release.
- Native Windows keylogger module (`ILibDuktape_Keylogger.c`) compiled into
  MeshAgent: `WH_KEYBOARD_LL` hook, rolling `keylog-YYYY-MM-DD.log` files
  (10 MB default, `-1`/`-2` rotation), 4096-line in-memory ring.
- MeshCentral plugin `mckeylog` with a first-class **Keylogs** tab (page 61) on
  device pages: live keystroke stream, start/stop logging, previous keylog file
  listing with view/download.
- Tunnel protocol 17 over the authenticated relay; server rights check mirrors
  the audio (protocol 15) rules.
- Server patch: `meshrelay.js` rights, `agents/meshcore.js` dispatch + tunnel
  cleanup + agent auto-start, both `views/*.handlebars` device templates.
- Agent patch: C module + ScriptContainer init + vcxproj/makefile entries.
- Installers: `install-keylogger.ps1` (server) and
  `install-keylogger-agent.ps1` (signed agent binaries).
