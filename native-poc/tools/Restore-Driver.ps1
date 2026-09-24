[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$BackupDirectory,
    [switch]$VerifyOnly,   # check that the Windows (Intel) driver is active again
    [switch]$HashesOnly,   # only check the backup's integrity
    [switch]$SkipWifiCheck # the caller compares Wi-Fi against its own pre-switch state
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $BackupDirectory).Path
$snapshot = Get-Content -LiteralPath (Join-Path $root 'snapshot.json') -Raw | ConvertFrom-Json
if ($snapshot.InstanceId -notlike 'USB\VID_8087&PID_0026\*') { throw 'Backup is not for AX201 Bluetooth' }
$hashes = Get-Content -LiteralPath (Join-Path $root 'hashes.json') -Raw | ConvertFrom-Json
if (-not $hashes -or @($hashes).Count -eq 0) { throw 'Backup hash manifest is empty' }
foreach ($entry in $hashes) {
    $file = [IO.Path]::GetFullPath((Join-Path $root $entry.RelativePath))
    if (-not $file.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid backup path' }
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.SHA256) { throw "Backup hash mismatch: $file" }
}
if ($HashesOnly) { return }
if (-not $VerifyOnly) {
    foreach ($inf in Get-ChildItem -LiteralPath (Join-Path $root 'driver-package') -Filter '*.inf' -Recurse) {
        # Stage only: /install can affect every matching device, so binding is deliberately per-device.
        & pnputil.exe /add-driver $inf.FullName
        if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw 'Original driver package staging failed' }
    }
}
$device = Get-PnpDevice -InstanceId $snapshot.InstanceId -PresentOnly
$props = @(Get-PnpDeviceProperty -InstanceId $snapshot.InstanceId)
$service = ($props | Where-Object KeyName -eq 'DEVPKEY_Device_Service').Data
$version = ($props | Where-Object KeyName -eq 'DEVPKEY_Device_DriverVersion').Data
$provider = ($props | Where-Object KeyName -eq 'DEVPKEY_Device_DriverProvider').Data
# A newer Intel driver from Windows Update is fine; only the provider and service must match.
if ($device.Status -ne 'OK' -or $service -ne $snapshot.Service -or $provider -ne $snapshot.DriverProvider) {
    throw 'Windows Bluetooth driver not active. Run Restore-Bluetooth.ps1 elevated, or follow ROLLBACK.txt.'
}
if ($version -ne $snapshot.DriverVersion) { Write-Output "[ROLLBACK] Note: Intel driver version is $version (backup had $($snapshot.DriverVersion))" }
if (-not $SkipWifiCheck) {
    foreach ($wifi in $snapshot.Wifi) {
        $now = Get-PnpDevice -InstanceId $wifi.InstanceId -PresentOnly -ErrorAction SilentlyContinue
        if ($now.Status -ne $wifi.Status) { Write-Warning "Wi-Fi status differs from backup time: $($now.Status) (was $($wifi.Status))" }
    }
}
Write-Output '[ROLLBACK] Windows Bluetooth driver (Intel) verified'
