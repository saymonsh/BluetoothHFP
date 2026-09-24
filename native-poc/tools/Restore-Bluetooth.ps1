# Emergency/boot-time restore of the Windows Bluetooth driver for the AX201.
# Uses only Windows' own tools: remove the device instance (drivers stay installed)
# and rescan, so Windows re-picks its best-ranked driver (Intel), never WinUSB.
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
if ($InstanceId -notlike 'USB\VID_8087&PID_0026\*') { Note "No valid AX201 instance id; nothing done"; exit 2 }

function Service() {
    try { return (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction Stop).Data } catch { return $null }
}
$before = Service
if ($before -eq 'BTHUSB') { Note "Bluetooth already on the Windows driver"; exit 0 }
Note "Bluetooth driver is '$before'; restoring the Windows driver"
& pnputil.exe /remove-device "$InstanceId" | Out-Null
& pnputil.exe /scan-devices | Out-Null
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Seconds 1
    if ((Service) -eq 'BTHUSB') { Note "Restored: Windows Bluetooth driver active"; exit 0 }
}
Note "Restore did not complete (driver now '$(Service)'). Restart Windows; if Bluetooth is still missing, Device Manager > Uninstall device > Scan for hardware changes."
exit 1
