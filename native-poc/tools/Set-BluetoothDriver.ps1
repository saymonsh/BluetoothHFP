# Switches ONLY the backed-up AX201 Bluetooth USB instance between Windows' Intel driver and WinUSB.
# Run elevated (Start-Q7.ps1 does this for you through a UAC prompt).
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('WinUSB','Intel')][string]$Mode,
    [string]$BackupDirectory
)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run elevated (as administrator)' }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$logDir = Join-Path $repo '.local\q7-run'
if (Test-Path -LiteralPath $logDir) { Start-Transcript -LiteralPath (Join-Path $logDir 'driver-switch.log') -Append | Out-Null }
if (-not $BackupDirectory) {
    $backup = Get-ChildItem (Join-Path $repo '.local\ax201-backup') -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
    if (-not $backup) { throw 'No driver backup. Run Backup-Driver.ps1 (elevated) while the Intel driver is active.' }
    $BackupDirectory = $backup.FullName
}
$root = (Resolve-Path -LiteralPath $BackupDirectory).Path
$snapshot = Get-Content -LiteralPath (Join-Path $root 'snapshot.json') -Raw | ConvertFrom-Json
if ($snapshot.InstanceId -notlike 'USB\VID_8087&PID_0026\*' -or $snapshot.Service -ne 'BTHUSB' -or
    $snapshot.DriverProvider -ne 'Intel Corporation' -or $snapshot.Inf -notmatch '^oem\d+\.inf$') { throw 'Not an original Intel AX201 backup' }
& (Join-Path $PSScriptRoot 'Restore-Driver.ps1') -BackupDirectory $root -HashesOnly
$helper = Join-Path $repo 'build\ax201-poc\Release\ax201_driver.exe'
if (-not (Test-Path -LiteralPath $helper)) { throw 'Build-Q7.ps1 must complete first' }
if (Get-Process q7_bridge -ErrorAction SilentlyContinue) { throw 'Stop the Q7 bridge before changing the driver' }

$instance = $snapshot.InstanceId
$service = (Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_Service).Data
if (($Mode -eq 'WinUSB' -and $service -eq 'WINUSB') -or ($Mode -eq 'Intel' -and $service -eq 'BTHUSB')) {
    Write-Output "[DRIVER] Already in $Mode mode"; exit 0
}
$originalInf = Join-Path $env:windir "INF\$($snapshot.Inf)"
$intelStaged = (Test-Path -LiteralPath $originalInf) -and
    ((Get-FileHash -LiteralPath $originalInf).Hash -eq (Get-FileHash -LiteralPath (Join-Path $root $snapshot.Inf)).Hash)
if ($Mode -eq 'WinUSB' -and -not $intelStaged) {
    throw 'The backed-up Intel driver package is no longer staged. Take a fresh backup (Backup-Driver.ps1) before switching.'
}
$wifiBefore = @{}
foreach ($wifi in $snapshot.Wifi) { $wifiBefore[$wifi.InstanceId] = (Get-PnpDevice -InstanceId $wifi.InstanceId -PresentOnly -ErrorAction SilentlyContinue).Status }

function Restore-Intel {
    if ($intelStaged) { & $helper install $instance $originalInf 'ibtusb'; if ($LASTEXITCODE -in 0, 3010) { return } }
    # Backed-up package missing or its install failed: let Windows pick its best driver.
    & (Join-Path $PSScriptRoot 'Restore-Bluetooth.ps1') -InstanceId $instance
    if ($LASTEXITCODE) { throw "Intel driver restore failed. Follow $root\ROLLBACK.txt" }
}

# The swap briefly leaves the device without a driver: ignore Ctrl+C until it is done.
$savedCtrlC = [Console]::TreatControlCAsInput
[Console]::TreatControlCAsInput = $true
try {
    if ($Mode -eq 'WinUSB') {
        & $helper list $instance $originalInf 'ibtusb'
        if ($LASTEXITCODE) { throw 'Original driver recovery candidate missing' }
        & $helper install $instance (Join-Path $env:windir 'INF\winusb.inf') 'WINUSB'
        if ($LASTEXITCODE -eq 3010) { throw 'Windows requires a reboot; automatic reboot is disabled' }
        if ($LASTEXITCODE) { throw 'WinUSB binding failed' }
    } else {
        Restore-Intel
        & (Join-Path $PSScriptRoot 'Restore-Driver.ps1') -BackupDirectory $root -VerifyOnly -SkipWifiCheck
    }
    foreach ($id in $wifiBefore.Keys) {
        if ((Get-PnpDevice -InstanceId $id -PresentOnly -ErrorAction SilentlyContinue).Status -ne $wifiBefore[$id]) { throw 'Wi-Fi PnP status changed' }
    }
    Write-Output "[DRIVER] $Mode mode active; Wi-Fi unchanged"
} catch {
    $failure = $_
    if ($Mode -eq 'WinUSB') {
        Write-Warning 'Switch failed. Restoring the Windows Bluetooth driver.'
        try { Restore-Intel } catch { Write-Warning $_ }
    }
    throw $failure
} finally {
    [Console]::TreatControlCAsInput = $savedCtrlC
}
