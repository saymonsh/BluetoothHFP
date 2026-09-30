# Switches how the AX201 carries call audio (SCO) under the Windows Bluetooth stack, for one
# decisive test: can two call-audio links (phone + headset) run at the same time?
#   Sideband - "Sco Support Type"=2 (how Windows runs it now): call audio goes over a side wire to
#              the Intel Smart Sound audio DSP, and Windows supports only ONE such link.
#   InBand   - "Sco Support Type"=1 (ScoSupportHCI; -InBandValue 0 = Intel's INF value, which gave no call audio here), where the
#              Windows stack allows "SCO Max Channels" links - unverified) plus "HfpOffloadDisable"=1.
#              The runtime 2 most likely comes from Intel's filter driver on this adapter (ibtusb.sys,
#              the only Intel driver here that holds these value names; the meaning of HfpOffloadDisable
#              is inferred from its strings, not documented).
# -Mode Status (default) only reads and runs as a normal user.
# -Mode InBand / Sideband CHANGE A SYSTEM SETTING (the adapter's key under
# HKLM\SYSTEM\CurrentControlSet\Enum\...\Device Parameters) and restart ONLY the Bluetooth adapter
# (Wi-Fi is a separate device): run them elevated. The original values are saved once, before the
# first change, in .local\sco-routing-backup.json.
# Undo: -Mode Sideband (without a usable backup it sets 2 and removes HfpOffloadDisable). While
# in-band, Windows call audio for Bluetooth headsets may not work.
# Log: .local\q7-run\sco-routing.log. Compatible with Windows PowerShell 5.1 and PowerShell 7.
[CmdletBinding()]
param([ValidateSet('Status','InBand','Sideband')][string]$Mode = 'Status',
      # 0 = Intel's INF value (on this PC it gave NO call audio at all, 2026-09-29); 1 = ScoSupportHCI, in-band by definition.
      [ValidateSet(0,1)][int]$InBandValue = 1)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$local = Join-Path $repo '.local'
$log = Join-Path $local 'q7-run\sco-routing.log'
$backupFile = Join-Path $local 'sco-routing-backup.json'
function Log([string]$text) { try { Add-Content -LiteralPath $log -Value "$(Get-Date -Format s) [$Mode] $text" } catch {} }
function Note([string]$text) { Write-Output "[SCO] $text"; Log $text }
trap { Log "FAILED: $_"; break }

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
$elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($Mode -ne 'Status' -and -not $elevated) { throw 'Run elevated (as administrator)' }
function Linked([string]$path) { # a junction/symlink (the link itself, not its target); missing = not linked
    try { return ([IO.File]::GetAttributes($path) -band [IO.FileAttributes]::ReparsePoint) -ne 0 } catch { return $false }
}
# .local is user-writable: a link planted there would redirect this elevated run's log and backup writes.
# ponytail: check-then-write race; the upgrade is keeping elevated files in C:\ProgramData\Q7Handsfree (admin-only write).
if ($elevated -and @($local, (Split-Path $log), $log, $backupFile | Where-Object { Linked $_ }).Count) {
    $log = $null # not even the FAILED line goes through it
    throw "A junction/symlink was found under $local; not writing through it as administrator"
}
if (-not (Test-Path -LiteralPath $local)) {
    # Same private ACL as Start-Q7.ps1. ponytail: assumes UAC elevates the same account (an admin
    # user); an over-the-shoulder login of another admin would get the folder instead.
    New-Item -ItemType Directory $local | Out-Null
    $me = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    & icacls.exe $local /inheritance:r /grant:r "*$($me):(OI)(CI)F" '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' | Out-Null
    if ($LASTEXITCODE) { throw 'Could not make .local private' }
}
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null

$radios = @(Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'USB\VID_8087&PID_0026\*')
if ($radios.Count -ne 1) { throw "Expected exactly one AX201 Bluetooth adapter; found $($radios.Count)" }
$instance = $radios[0].InstanceId
$key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$instance\Device Parameters"
# The values saved/restored, with upper bounds. No INF sets HfpOffloadDisable; only InBand writes it.
$maximum = [ordered]@{ 'Sco Support Type' = 2; 'SCO Max Channels' = 9; 'HfpOffloadDisable' = 1 }
$meaning = @{
    0 = 'Intel''s INF default; assumed in-band (name ScoSupportNone) - unverified'
    1 = 'in-band over HCI (ScoSupportHCI)'
    2 = 'sideband to the Intel Smart Sound DSP; Windows allows ONE such call-audio link'
}

function Service() {
    try { return (Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_Service -ErrorAction Stop).Data } catch { return $null }
}
function Healthy() {
    try { return (Service) -eq 'BTHUSB' -and (Get-PnpDevice -InstanceId $instance -ErrorAction Stop).Status -eq 'OK' } catch { return $false }
}
function Read-Value([string]$name) { return (Get-Item -LiteralPath $key).GetValue($name) }
function Describe([string]$name, $value) {
    if ($null -eq $value) { return 'not set' }
    if ($name -eq 'Sco Support Type' -and $value -is [int] -and $meaning.Contains($value)) { return "$value ($($meaning[$value]))" }
    return "$value"
}
function Bounded([string]$name, $value) { return "$value" -match '^\d$' -and [int]"$value" -le $maximum[$name] }
function Read-Backup() {
    if (-not (Test-Path -LiteralPath $backupFile)) { return $null }
    # Read back elevated from a user-writable folder: accept only what this script writes.
    $saved = Get-Content -LiteralPath $backupFile -Raw | ConvertFrom-Json
    if ($saved.InstanceId -ne $instance) { throw "$backupFile is for another adapter instance; check it by hand" }
    $when = $saved.SavedAt -as [datetime] # PS 7 has already parsed it; 5.1 keeps the string
    if ($null -eq $when) { throw "$backupFile is damaged ('SavedAt'); check it by hand" }
    $saved.SavedAt = $when
    foreach ($name in $maximum.Keys) {
        $entry = $saved.Original.$name
        if ($entry.Existed -isnot [bool] -or ($entry.Existed -and -not (Bounded $name $entry.Value))) {
            throw "$backupFile is damaged ('$name'); check it by hand"
        }
    }
    return $saved
}
function Original($saved, [string]$name) { # $null = the value did not exist
    $entry = $saved.Original.$name
    if ($entry.Existed) { return [int]"$($entry.Value)" } else { return $null }
}

$service = Service
Note "Adapter: $instance"
Note "Driver: $service ($(if ($service -eq 'BTHUSB') { 'Windows' } elseif ($service -eq 'WINUSB') { 'lent to the Q7 bridge' } else { 'unexpected' }))"
foreach ($name in $maximum.Keys) { Note "'$name' = $(Describe $name (Read-Value $name))" }
$sst = @(Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'INTELAUDIO\CTLR_DEV_*&LINKTYPE_03*')
if (-not $sst) { Note 'Intel Smart Sound Bluetooth Audio device: not present' }
foreach ($device in $sst) { Note "Intel Smart Sound Bluetooth Audio device: $($device.Status) ($($device.InstanceId))" }
if ($Mode -eq 'Sideband') {
    # Giving call audio back must never be blocked by a damaged backup: fall back to the Windows value.
    try { $saved = Read-Backup } catch { $saved = $null; Note "The saved values in $backupFile cannot be used; left the file for inspection" }
} else { $saved = Read-Backup }
if ($saved) {
    Note "Saved original values ($($saved.SavedAt.ToString('s'))), put back by -Mode Sideband:"
    foreach ($name in $maximum.Keys) { Note "  '$name' = $(Describe $name (Original $saved $name))" }
} elseif ($Mode -ne 'Sideband') { Note 'No saved original values yet (the first -Mode InBand saves them)' }
if ($Mode -eq 'Status') { return }

if ($service -ne 'BTHUSB') { throw "The adapter is not on the Windows driver ('$service'). Let Start-Q7.ps1 finish, or run Restore-Bluetooth.ps1 elevated." }
if (Get-Process q7_bridge -ErrorAction SilentlyContinue) { throw 'Stop the Q7 bridge first' }
if ($Mode -eq 'InBand') {
    if (-not $saved) {
        $registry = Get-Item -LiteralPath $key
        $original = [ordered]@{}
        foreach ($name in $maximum.Keys) {
            $existed = $registry.GetValueNames() -contains $name
            if ($existed -and ($registry.GetValueKind($name) -ne 'DWord' -or -not (Bounded $name $registry.GetValue($name)))) {
                throw "'$name' is not a DWORD from 0 to $($maximum[$name]); not touching it"
            }
            $original[$name] = [ordered]@{ Existed = $existed; Value = $registry.GetValue($name) }
        }
        # Only the Windows value is a real original: Intel's INF writes 0 whenever its driver is
        # reinstalled (e.g. at the end of a bridge session), and saving that would make Sideband "restore" 0.
        $current = $original['Sco Support Type'].Value
        if ($current -ne 2) { throw "'Sco Support Type' is already $(Describe 'Sco Support Type' $current), not the Windows sideband value 2; restart Windows and run -Mode Status first" }
        # -NoClobber: the backup must keep the values from before the FIRST change.
        [ordered]@{ InstanceId = $instance; SavedAt = (Get-Date).ToString('o'); Original = $original } |
            ConvertTo-Json -Depth 4 | Out-File -LiteralPath $backupFile -Encoding utf8 -NoClobber
        Note "Saved the original values to $backupFile"
    }
    $target = [ordered]@{ 'Sco Support Type' = $InBandValue; 'HfpOffloadDisable' = 1 }
} elseif ($saved) {
    $target = [ordered]@{}
    foreach ($name in $maximum.Keys) { $target[$name] = Original $saved $name }
} else {
    Note 'No usable saved values; setting the Windows sideband value 2 and removing HfpOffloadDisable'
    $target = [ordered]@{ 'Sco Support Type' = 2; 'HfpOffloadDisable' = $null }
}
foreach ($name in $target.Keys) {
    if ($null -ne $target[$name]) { New-ItemProperty -LiteralPath $key -Name $name -PropertyType DWord -Value $target[$name] -Force | Out-Null }
    elseif ((Get-Item -LiteralPath $key).GetValueNames() -contains $name) { Remove-ItemProperty -LiteralPath $key -Name $name }
    Note "Set '$name' to $(Describe $name ($target[$name]))"
}
$warning = 'WARNING: in in-band mode Windows call audio for Bluetooth headsets may stop working. Undo with -Mode Sideband.'

# The Bluetooth stack reads these values when the adapter starts: restart only this device (and its Bluetooth children).
Note 'Restarting the Bluetooth adapter...'
if ((& pnputil.exe /? | Out-String) -match '/restart-device') {
    $output = & pnputil.exe /restart-device "$instance" | Out-String
    if ($LASTEXITCODE -eq 3010) {
        Note 'Windows could not restart the adapter live. Restart Windows to apply the change, then run -Mode Status.'
        if ($Mode -eq 'InBand') { Note $warning }
        exit 3010
    }
    if ($LASTEXITCODE) {
        $next = if ($Mode -eq 'InBand') { " $warning" } else { '' }
        throw "pnputil could not restart the adapter (exit $LASTEXITCODE): $output Restart Windows, then run -Mode Status to see what it holds.$next"
    }
} else {
    try { Disable-PnpDevice -InstanceId $instance -Confirm:$false } finally { Enable-PnpDevice -InstanceId $instance -Confirm:$false }
}
for ($i = 0; $i -lt 30 -and -not (Healthy); $i++) { Start-Sleep -Seconds 1 }
$healthy = Healthy
if (-not $healthy) { Note "WARNING: the adapter is not back after 30 s (driver '$(Service)'). Restart Windows if Bluetooth stays missing." }
Start-Sleep -Seconds 5 # ponytail: catches a rewrite made while the adapter starts; a later one only shows in -Mode Status

$persisted = $true
foreach ($name in $target.Keys) {
    $now = Read-Value $name
    if ($now -eq $target[$name]) { Note "Persisted after the restart: '$name' = $(Describe $name $now)" }
    else {
        $persisted = $false
        Note "REWRITTEN at runtime: '$name' now holds $(Describe $name $now), not $(Describe $name ($target[$name])). Something sets it when the adapter starts (most likely Intel's filter driver ibtusb.sys; the Smart Sound audio driver does not hold this value name)."
    }
}
if ($Mode -eq 'InBand') {
    Note $warning
    if ($persisted) { Note 'In-band routing is set. Run the two-call-audio tests now.' }
    else { Note 'In-band routing did NOT stick, so this test cannot run this way. Undo with -Mode Sideband.' }
} elseif ($persisted) { Note 'Original call-audio routing restored' }
if (-not ($persisted -and $healthy)) { exit 1 }
