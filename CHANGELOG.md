# Changelog

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
