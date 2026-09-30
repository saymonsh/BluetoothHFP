# Turns Windows' "Handsfree Telephony" service of ONE paired headset off or on, for
# `q7_winstack route --stereo` (the headset as stereo headphones + the PC microphone).
# Why: this laptop carries ONE call-audio link. While the headset offers hands-free, Windows sends a
# call moved to the PC to the headset's hands-free link, which needs a second one; that fails and the
# call audio silently returns to the phone within ~2 s. With the service off, Windows sees the headset
# as stereo headphones only and the call stays on the PC.
# Nothing is unpaired: only this service is switched (same as the checkbox under Control Panel >
# Devices and Printers > the headset > Properties > Services). Runs as a normal user.
# While off, PC call apps (Teams, Zoom...) cannot use the headset's microphone. Undo: -State On.
#   Set-HeadsetHandsFree.ps1 -Address 88:0E:85:20:16:C9 -State Off
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Address,
      [Parameter(Mandatory)][ValidateSet('Off','On')][string]$State)
$ErrorActionPreference = 'Stop'
$hex = $Address -replace '[:-]', ''
if ($hex -notmatch '^[0-9A-Fa-f]{12}$') { throw "Not a Bluetooth address: $Address" }
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class BtService {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DeviceInfo {
        public uint dwSize; public ulong Address; public uint ClassOfDevice;
        public int Connected, Remembered, Authenticated;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public byte[] LastSeen;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public byte[] LastUsed;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 248)] public string Name;
    }
    [DllImport("bthprops.cpl")] public static extern uint BluetoothGetDeviceInfo(IntPtr radio, ref DeviceInfo info);
    [DllImport("bthprops.cpl")] public static extern uint BluetoothSetServiceState(IntPtr radio, ref DeviceInfo info, ref Guid service, uint flags);
}
'@
$info = New-Object BtService+DeviceInfo
$info.dwSize = [uint32][Runtime.InteropServices.Marshal]::SizeOf([type][BtService+DeviceInfo])
$info.Address = [Convert]::ToUInt64($hex, 16)
$rc = [BtService]::BluetoothGetDeviceInfo([IntPtr]::Zero, [ref]$info)
if ($rc -ne 0) { throw "$Address is not paired with Windows (error $rc)" }
if (-not $info.Remembered) { throw "$Address ($($info.Name)) is not paired with Windows" }
$handsFree = [Guid]'0000111e-0000-1000-8000-00805f9b34fb' # Hands-Free Profile, hands-free unit side
$rc = [BtService]::BluetoothSetServiceState([IntPtr]::Zero, [ref]$info, [ref]$handsFree, [uint32]($State -eq 'On'))
# Windows reports a service already in the asked state as an error: ERROR_NOT_FOUND for Off.
# ponytail: ERROR_INVALID_PARAMETER for On is ALSO what it returned when already on (2026-09-30), so
# it is taken as "already on"; a real refusal with the same code would be misreported.
if ($rc -eq 1168 -and $State -eq 'Off') { Write-Output "[HFP] $($info.Name) ($Address): Handsfree Telephony already Off"; exit 0 }
if ($rc -eq 87 -and $State -eq 'On') { Write-Output "[HFP] $($info.Name) ($Address): Handsfree Telephony already On"; exit 0 }
if ($rc -ne 0) { throw "Windows refused to switch hands-free $State for $($info.Name) (error $rc)" }
Write-Output "[HFP] $($info.Name) ($Address): Handsfree Telephony $State"
