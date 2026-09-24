[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot '..\..\.local\ax201-backup' }
$devices = @(Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'USB\VID_8087&PID_0026\*')
if ($devices.Count -ne 1) { throw "Expected exactly one physical AX201 Bluetooth device; found $($devices.Count)" }
$device = $devices[0]
$properties = @(Get-PnpDeviceProperty -InstanceId $device.InstanceId)
function PropertyValue([string]$Name) {
    ($properties | Where-Object KeyName -eq $Name).Data
}
$inf = PropertyValue 'DEVPKEY_Device_DriverInfPath'
if ($inf -notmatch '^oem\d+\.inf$') { throw "Expected an exportable OEM INF, got: $inf" }
$outputRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory) # relative to $PWD
$destination = Join-Path $outputRoot (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Path $destination | Out-Null
$package = New-Item -ItemType Directory -Path (Join-Path $destination 'driver-package')
$net = @(Get-NetAdapter | Select-Object Name,InterfaceDescription,Status,InterfaceGuid)
$wifi = @(Get-PnpDevice -Class Net -PresentOnly | Where-Object FriendlyName -match 'AX201')
$snapshot = [ordered]@{
    CapturedAt = (Get-Date).ToString('o')
    InstanceId = $device.InstanceId
    FriendlyName = $device.FriendlyName
    Inf = $inf
    Service = PropertyValue 'DEVPKEY_Device_Service'
    DriverVersion = PropertyValue 'DEVPKEY_Device_DriverVersion'
    DriverProvider = PropertyValue 'DEVPKEY_Device_DriverProvider'
    LowerFilters = @(PropertyValue 'DEVPKEY_Device_LowerFilters')
    Parent = PropertyValue 'DEVPKEY_Device_Parent'
    Wifi = @($wifi | Select-Object InstanceId,FriendlyName,Status)
    Network = $net
}
$snapshot | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $destination 'snapshot.json') -Encoding UTF8
$properties | Export-Clixml (Join-Path $destination 'device-properties.xml')
Copy-Item -LiteralPath (Join-Path $env:windir "INF\$inf") -Destination (Join-Path $destination $inf)
$export = & pnputil.exe /export-driver $inf $package.FullName 2>&1
$export | Set-Content (Join-Path $destination 'export.txt')
if ($LASTEXITCODE -ne 0) { throw "Driver export failed. No driver changes permitted. See $destination" }
$infs = @(Get-ChildItem $package.FullName -Filter '*.inf' -Recurse)
if ($infs.Count -eq 0 -or @(Get-ChildItem $package.FullName -Filter '*.sys' -Recurse).Count -eq 0 -or
    @(Get-ChildItem $package.FullName -Filter '*.cat' -Recurse).Count -eq 0) {
    throw 'Export is incomplete: expected INF, SYS and CAT files'
}
Get-ChildItem $package.FullName -File -Recurse | ForEach-Object {
    [pscustomobject]@{ RelativePath = $_.FullName.Substring($destination.Length + 1); SHA256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
} | ConvertTo-Json | Set-Content (Join-Path $destination 'hashes.json') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Restore-Driver.ps1') -Destination $destination
@"
AX201 Bluetooth driver recovery (no Wi-Fi driver changes)
Target instance: $($device.InstanceId)
Original provider/version: $($snapshot.DriverProvider) / $($snapshot.DriverVersion)
Original published INF: $inf
Exported INF(s):
$($infs.FullName -join "`r`n")

1. Use a local keyboard/mouse that does not depend on Bluetooth.
2. Open Device Manager, find the device by the EXACT instance above under
   Properties > Details > Device instance path (its category/name may change).
3. Update driver > Browse my computer > Let me pick > Have Disk.
   Select the exported Intel INF above and Intel(R) Wireless Bluetooth(R).
   Do not select the PCI Wi-Fi AX201 device, USB hub or host controller.
4. Restart Windows if requested. Then run:
   powershell -File .\Restore-Driver.ps1 -BackupDirectory . -VerifyOnly
5. If the export needs staging first, run Restore-Driver.ps1 without -VerifyOnly
   from an elevated PowerShell. Staging does NOT force a lower-ranked driver;
   repeat Have Disk if verification still fails.

Do not delete oem packages, clear filters manually, disable Secure Boot, enable
test signing, or uninstall the USB parent to recover this Bluetooth function.
The backup is verified on disk; re-binding recovery has not been exercised.
"@ | Set-Content (Join-Path $destination 'ROLLBACK.txt') -Encoding UTF8
Write-Output "[BACKUP] Current driver: $inf / $($snapshot.DriverVersion)"
Write-Output "[BACKUP] INF/SYS/CAT exported and SHA256 recorded: $destination"
Write-Output '[BACKUP] Rollback instructions saved; no driver binding changed'
