# Emergency/boot-time restore of the Windows Bluetooth driver for the AX201.
# Uses only Windows' own tools: remove the device instance (drivers stay installed)
# and rescan, so Windows re-picks its best-ranked driver (Intel), never WinUSB.
# Then re-enables the Bluetooth audio devices (and Intel's Bluetooth Audio device) that
# Set-BluetoothDriver.ps1 disabled to free the adapter.
# Compatible with Windows PowerShell 5.1 (the boot task runs it as SYSTEM).
[CmdletBinding()]
param([string]$InstanceId)
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot 'restore.log'
function Note([string]$text) {
    $line = "$(Get-Date -Format s) $text"
    Write-Output $line
    try { Add-Content -LiteralPath $log -Value $line } catch {}
}
if (-not $InstanceId) {
    $saved = Join-Path $PSScriptRoot 'instance.txt'
    if (Test-Path -LiteralPath $saved) { $InstanceId = (Get-Content -LiteralPath $saved -Raw).Trim() }
}
if (-not $InstanceId) {
    # Run by hand from the repo: find the (single) AX201 Bluetooth function by its hardware ID.
    $InstanceId = (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object InstanceId -like 'USB\VID_8087&PID_0026\*' | Select-Object -First 1).InstanceId
}
if ($InstanceId -notlike 'USB\VID_8087&PID_0026\*') { Note "No valid AX201 instance id; nothing done"; exit 2 }

function Service() {
    try { return (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction Stop).Data } catch { return $null }
}
function Healthy() {
    try { return (Service) -eq 'BTHUSB' -and (Get-PnpDevice -InstanceId $InstanceId -ErrorAction Stop).Status -eq 'OK' } catch { return $false }
}
# Records of disabled audio devices. The boot task (SYSTEM) reads only its own admin-only folder;
# run from the repo, the repo's record is read too. Lines are validated either way: only Bluetooth
# (BTHENUM) devices and Intel's Bluetooth Audio device (SST link type 3) are ever enabled.
$bootFolder = Join-Path $env:ProgramData 'Q7Handsfree'
$records = @(Join-Path $bootFolder 'disabled-devnodes.txt')
if ($PSScriptRoot -ne $bootFolder) { $records += Join-Path $PSScriptRoot '..\..\.local\q7-run\disabled-devnodes.txt' }
function Finish([int]$code) {
    # ponytail: one shared 20 s wait for the devices to reappear after the Windows driver starts; whatever
    # cannot be re-enabled stays recorded and is retried on the next restore or Windows start.
    $deadline = (Get-Date).AddSeconds(20)
    foreach ($record in $records) {
        if (-not (Test-Path -LiteralPath $record)) { continue }
        $pending = @()
        foreach ($id in @(Get-Content -LiteralPath $record | Where-Object { $_ } | Select-Object -Unique)) {
            if ($id -notmatch '^(BTHENUM\\|INTELAUDIO\\CTLR_DEV_[0-9A-F]{4}&LINKTYPE_03&)[^\s"]{1,200}$') { Note 'Ignored an invalid line in the disabled-device record'; continue }
            if (-not (Get-PnpDevice -InstanceId $id -ErrorAction SilentlyContinue)) { Note "No longer exists (unpaired?): $id"; continue }
            while (-not (Get-PnpDevice -InstanceId $id -PresentOnly -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
            & pnputil.exe /enable-device $id | Out-Null
            if ($LASTEXITCODE -in 0, 3010) { Note "Re-enabled $id" } else { Note "Could not re-enable $id yet (pnputil $LASTEXITCODE)"; $pending += $id }
        }
        if ($pending.Count) { Set-Content -LiteralPath $record -Value $pending -Encoding ascii } else { Remove-Item -LiteralPath $record }
    }
    exit $code
}
$before = Service
if (Healthy) { Note "Bluetooth already working on the Windows driver"; Finish 0 }
Note "Bluetooth driver is '$before'; restoring the Windows driver"
& pnputil.exe /remove-device "$InstanceId" | Out-Null
& pnputil.exe /scan-devices | Out-Null
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Seconds 1
    if (Healthy) { Note "Restored: Windows Bluetooth driver active"; Finish 0 }
}
Note "Restore did not complete (driver now '$(Service)'). Restart Windows; if Bluetooth is still missing, Device Manager > Uninstall device > Scan for hardware changes."
Finish 1
