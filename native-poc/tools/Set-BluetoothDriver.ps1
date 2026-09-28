# Switches ONLY the backed-up AX201 Bluetooth USB instance between Windows' Intel driver and WinUSB.
# Run elevated (Start-Q7.ps1 does this for you through a UAC prompt).
# Pairings stay shared with the bridge, so no device has to be removed anywhere:
#  - WinUSB: Windows' link keys for phones/headsets are copied to the bridge's private run folder (windows-keys.txt).
#    If Windows cannot let go of the adapter because something holds a Bluetooth audio device open, the
#    devices holding it are disabled instead (recorded first) and the switch is retried: first this radio's
#    audio devices, then Intel's "Bluetooth Audio" (SST) device.
#  - Intel: a key the bridge created for its phone/headset (bridge-new-keys.txt) replaces Windows' key for
#    that device if Windows still holds the key the bridge replaced, before the Intel driver starts;
#    afterwards Restore-Bluetooth.ps1 re-enables the disabled devices.
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
try { if (Test-Path -LiteralPath $logDir) { Start-Transcript -LiteralPath (Join-Path $logDir 'driver-switch.log') -Append | Out-Null } } catch {}
if (-not $BackupDirectory) {
    $backup = Get-ChildItem (Join-Path $repo '.local\ax201-backup') -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
    if ($backup) { $BackupDirectory = $backup.FullName }
}
if ($Mode -eq 'Intel' -and -not $BackupDirectory) {
    # Giving Bluetooth back must never be blocked by a missing/damaged backup.
    & (Join-Path $PSScriptRoot 'Restore-Bluetooth.ps1'); exit $LASTEXITCODE
}
if (-not $BackupDirectory) { throw 'No driver backup. Run Backup-Driver.ps1 (elevated) while the Intel driver is active.' }
$root = (Resolve-Path -LiteralPath $BackupDirectory).Path
$snapshot = Get-Content -LiteralPath (Join-Path $root 'snapshot.json') -Raw | ConvertFrom-Json
if ($snapshot.InstanceId -notlike 'USB\VID_8087&PID_0026\*' -or $snapshot.Service -ne 'BTHUSB' -or
    $snapshot.DriverProvider -ne 'Intel Corporation' -or $snapshot.Inf -notmatch '^oem\d+\.inf$') { throw 'Not an original Intel AX201 backup' }
$instance = $snapshot.InstanceId
$helper = Join-Path $repo 'build\ax201-poc\Release\ax201_driver.exe'
if (Get-Process q7_bridge -ErrorAction SilentlyContinue) {
    if ($Mode -eq 'WinUSB') { throw 'Stop the Q7 bridge before changing the driver' }
    Get-Process q7_bridge | Stop-Process -Force; Start-Sleep -Seconds 1 # restoring wins over a stuck bridge
}
$bootFolder = Join-Path $env:ProgramData 'Q7Handsfree' # boot safety net (Install-BootSafety.ps1), admin-only
# Only if it really is that folder: not a link, and owned by Administrators (a normal user can create a
# folder in ProgramData first, but cannot give it that owner).
function Test-BootFolder {
    try {
        return -not ((Get-Item -LiteralPath $bootFolder -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -and
            (Get-Acl -LiteralPath $bootFolder).GetOwner([Security.Principal.SecurityIdentifier]).Value -eq 'S-1-5-32-544'
    } catch { return $false }
}
# Name of this adapter's key folder in Windows (BTHPORT\Parameters\Keys\<address>), cached by Windows.
$adapter = "$((Get-ItemProperty -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Enum\$instance\Device Parameters" -ErrorAction SilentlyContinue).DeviceAddressCache)".ToLowerInvariant()

# Bridge -> Windows. Only a phone/headset Windows has already paired gets the new key, and only while
# Windows still holds the key the bridge replaced (never a new Windows pairing, never over a newer one).
# The file is user-writable, so the helper parses it strictly. Never blocks the switch.
function Push-BridgeKeys {
    $file = Join-Path $logDir 'bridge-new-keys.txt'
    if (-not (Test-Path -LiteralPath $file)) { return $true }
    try {
        if ($adapter -notmatch '^[0-9a-f]{12}$' -or -not (Test-Path -LiteralPath $helper)) { throw 'adapter address or helper missing' }
        & $helper import-keys $adapter $file | Out-Host
        if ($LASTEXITCODE -eq 65) {
            # Unreadable (e.g. cut short when a bridge was killed): set aside, or it would block key sharing for good.
            Move-Item -LiteralPath $file -Destination (Join-Path $logDir 'bridge-new-keys.rejected.txt') -Force
            Write-Warning 'Bridge keys were unreadable and set aside (bridge-new-keys.rejected.txt); Windows keeps its own keys'
            return $true
        }
        if ($LASTEXITCODE) { throw "helper exit $LASTEXITCODE" }
        Remove-Item -LiteralPath $file
        return $true
    } catch { Write-Warning "Bridge keys were not copied to Windows ($_); $file is kept for the next switch"; return $false }
}
# Windows -> bridge, into the private run folder Start-Q7.ps1 created (the file inherits its ACL).
function Export-WindowsKeys {
    $file = Join-Path $logDir 'windows-keys.txt'
    if (-not (Test-Path -LiteralPath $logDir) -or $adapter -notmatch '^[0-9a-f]{12}$') { Write-Warning 'Windows pairings are not shared with the bridge this time'; return }
    & $helper export-keys $adapter $file
    if ($LASTEXITCODE) {
        Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
        Write-Warning "Windows pairings are not shared with the bridge this time (exit $LASTEXITCODE); it uses its own keys"
    }
}
# Paired audio devices of THIS radio (BTHENUM nodes under its "Microsoft Bluetooth Enumerator" child):
# hands-free, audio gateway, A2DP sink/source, headset, headset AG; AVRCP only if it is the vetoer.
function Get-AudioDevnodes([string]$vetoer) {
    $nodes = @((Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_Children).Data | Where-Object { $_ })
    $nodes += @($nodes | ForEach-Object { (Get-PnpDeviceProperty -InstanceId $_ -KeyName DEVPKEY_Device_Children -ErrorAction SilentlyContinue).Data } | Where-Object { $_ })
    $nodes | Where-Object {
        $_ -match '^BTHENUM\\\{0000(111E|111F|110B|110A|1108|1112)-0000-1000-8000-00805F9B34FB\}' -or
        ($_ -eq $vetoer -and $_ -match '^BTHENUM\\\{0000(110E|110C)-0000-1000-8000-00805F9B34FB\}') }
}
# Intel's "Bluetooth Audio" (SST) device opens the sideband (HCIBYPASS) hands-free device of a headset.
# The observed veto (open handle on that device) stayed with Windows' audio services stopped, so it is
# most likely this kernel-mode holder. Restore-Bluetooth.ps1 accepts exactly this pattern besides BTHENUM.
function Get-SstBluetoothAudio {
    (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object InstanceId -match '^INTELAUDIO\\CTLR_DEV_[0-9A-F]{4}&LINKTYPE_03&').InstanceId
}
# Disables devices until Bluetooth is given back. Only running ones (a device the user disabled stays so),
# each recorded BEFORE it is disabled, so the Intel restore and the boot task can re-enable it. A disable
# that cannot happen live (pnputil 3010: pending until restart, the device is held open) is undone at once.
# Returns the ids that were really disabled.
function Disable-Devices([string[]]$ids) {
    $ids = @($ids | Where-Object { $_ -and (Get-PnpDevice -InstanceId $_ -PresentOnly -ErrorAction SilentlyContinue).Status -eq 'OK' } | Select-Object -Unique)
    if (-not $ids.Count) { return }
    $record = if (Test-BootFolder) { Join-Path $bootFolder 'disabled-devnodes.txt' } else {
        New-Item -ItemType Directory -Force $logDir | Out-Null; Join-Path $logDir 'disabled-devnodes.txt' }
    Add-Content -LiteralPath $record -Value $ids -Encoding ascii
    foreach ($id in $ids) {
        & pnputil.exe /disable-device $id | Out-Host
        if ($LASTEXITCODE -eq 0) { Write-Host "[DRIVER] Disabled until Bluetooth is given back: $id"; $id; continue }
        $code = $LASTEXITCODE
        & pnputil.exe /enable-device $id | Out-Host
        Write-Host "[DRIVER] Could not disable live (pnputil $code); left enabled: $id"
    }
}
if ($Mode -eq 'WinUSB') {
    # Checks that only matter before lending the adapter out.
    & (Join-Path $PSScriptRoot 'Restore-Driver.ps1') -BackupDirectory $root -HashesOnly
    if (-not (Test-Path -LiteralPath $helper)) { throw 'Build-Q7.ps1 must complete first' }
}

$service = (Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_Service).Data
if ($Mode -eq 'WinUSB' -and $service -eq 'WINUSB') { Write-Output '[DRIVER] Already in WinUSB mode'; exit 0 }
if ($Mode -eq 'Intel') { [void](Push-BridgeKeys) } # before Windows' Bluetooth starts again and reads its keys
if ($Mode -eq 'Intel' -and $service -eq 'BTHUSB') {
    try { & (Join-Path $PSScriptRoot 'Restore-Driver.ps1') -BackupDirectory $root -VerifyOnly -SkipWifiCheck } # throws if not working
    finally { & (Join-Path $PSScriptRoot 'Restore-Bluetooth.ps1') -InstanceId $instance } # re-enables devices disabled for the bridge, even then
    exit 0
}
$originalInf = Join-Path $env:windir "INF\$($snapshot.Inf)"
$intelStaged = (Test-Path -LiteralPath $originalInf) -and
    ((Get-FileHash -LiteralPath $originalInf).Hash -eq (Get-FileHash -LiteralPath (Join-Path $root $snapshot.Inf)).Hash)
if ($Mode -eq 'WinUSB' -and -not $intelStaged) {
    throw 'The backed-up Intel driver package is no longer staged. Take a fresh backup (Backup-Driver.ps1) before switching.'
}
# Windows is the source of truth: first give it a key the bridge created last time (if still pending),
# then hand all of its keys to the bridge.
if ($Mode -eq 'WinUSB' -and (Push-BridgeKeys)) { Export-WindowsKeys }
$wifiBefore = @{}
foreach ($wifi in $snapshot.Wifi) { $wifiBefore[$wifi.InstanceId] = (Get-PnpDevice -InstanceId $wifi.InstanceId -PresentOnly -ErrorAction SilentlyContinue).Status }

function Restore-Intel {
    if ($intelStaged -and (Test-Path -LiteralPath $helper)) {
        & $helper install $instance $originalInf 'ibtusb'; if ($LASTEXITCODE -in 0, 3010) { return }
    }
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
        $winusb = Join-Path $env:windir 'INF\winusb.inf'
        & $helper install $instance $winusb 'WINUSB'
        $result = $LASTEXITCODE
        # Vetoed: something holds a Bluetooth audio device of this radio open, usually for a headset paired with
        # Windows. Instead of unpairing anything, disable the holders until Bluetooth is given back, retrying the
        # switch after each step that disabled something: first this radio's audio devices, then, if a sideband
        # (HCIBYPASS) hands-free device still vetoes, Intel's Bluetooth Audio device that keeps it open.
        # ponytail: Plug and Play names only the first vetoer, so an unlisted holder still fails after the two
        # steps (the error names it); a veto -> disable -> retry loop over any holder would be the upgrade.
        foreach ($step in 1, 2) {
            if ($result -ne 3010) { break }
            $veto = "$(& $helper veto $instance)" # if no longer vetoed, this already re-binds the adapter
            $vetoed = $LASTEXITCODE -ne 0
            Write-Output $veto
            $vetoer = if ($veto -match 'vetoName=(\S+)') { $Matches[1] }
            $freed = @(if ($vetoed -and $step -eq 1) { Disable-Devices @(Get-AudioDevnodes $vetoer) }
                       elseif ($vetoed -and $vetoer -match '^BTHENUM\\.*_HCIBYPASS_') { Disable-Devices @(Get-SstBluetoothAudio) })
            if ($freed.Count -or -not $vetoed) { & $helper install $instance $winusb 'WINUSB'; $result = $LASTEXITCODE }
        }
        if ($result -eq 3010) {
            throw "Windows could not release the Bluetooth adapter live (a device keeps it open, see below). Nothing has to be removed or unpaired: disconnect or turn off the Bluetooth headset/speaker and try again. $(& $helper veto $instance)"
        }
        if ($result) { throw 'WinUSB binding failed' }
    } else {
        try {
            Restore-Intel
            & (Join-Path $PSScriptRoot 'Restore-Driver.ps1') -BackupDirectory $root -VerifyOnly -SkipWifiCheck
        } finally { & (Join-Path $PSScriptRoot 'Restore-Bluetooth.ps1') -InstanceId $instance } # re-enables devices disabled for the bridge, even if a check failed
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
        # Also when a disable was undone at once: its id is still recorded and gets cleared here.
        try { & (Join-Path $PSScriptRoot 'Restore-Bluetooth.ps1') -InstanceId $instance } catch { Write-Warning $_ }
    }
    throw $failure
} finally {
    [Console]::TreatControlCAsInput = $savedCtrlC
}
