# Runs one Q7 bridge session. Start it as a NORMAL user (not "Run as administrator").
# Windows asks for permission (UAC) twice: once to lend the Bluetooth adapter to the
# bridge, once to give it back. The bridge itself runs without admin rights.
# Whatever happens (quit, error, crash), the Windows Bluetooth driver is restored at the end.
[CmdletBinding()]
param([switch]$DebugPacketLog)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$tools = $PSScriptRoot
$exe = Join-Path $repo 'build\q7\Release\q7_bridge.exe'
$probe = Join-Path $repo 'build\ax201-poc\Release\ax201_probe.exe'
foreach ($f in $exe, $probe) { if (-not (Test-Path -LiteralPath $f)) { throw "Missing $f. Run Build-Q7.ps1 first." } }
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Start this from a normal (non-administrator) PowerShell window; it will ask for permission when needed.'
}
$backup = Get-ChildItem (Join-Path $repo '.local\ax201-backup') -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
if (-not $backup) { throw 'No driver backup yet. Run Backup-Driver.ps1 once in an administrator window.' }
$instance = (Get-Content -LiteralPath (Join-Path $backup.FullName 'snapshot.json') -Raw | ConvertFrom-Json).InstanceId
if ($instance -notlike 'USB\VID_8087&PID_0026\*') { throw 'Backup does not describe the AX201 Bluetooth adapter' }
# Device path fragment used by the bridge to open only this adapter, e.g. vid_8087&pid_0026#5&ba996e2&0&10#
$fragment = (($instance -replace '^USB\\', '') -replace '\\', '#').ToLowerInvariant() + '#'

# Private working folder: link keys, saved devices and the log are readable by this user only.
$run = Join-Path $repo '.local\q7-run'
New-Item -ItemType Directory -Force $run | Out-Null
& icacls.exe (Join-Path $repo '.local') /inheritance:r /grant:r "$($env:USERNAME):(OI)(CI)F" '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' | Out-Null
if ($LASTEXITCODE) { throw 'Could not make .local private' }

$shell = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe' # built-in, always present for elevation
function Invoke-Elevated([string]$script, [string[]]$arguments) {
    $argumentList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $tools $script)) + $arguments
    $process = Start-Process -FilePath $shell -Verb RunAs -Wait -PassThru -ArgumentList $argumentList
    return $process.ExitCode
}
function Restore-WindowsBluetooth {
    Write-Output '[Q7] Giving the Bluetooth adapter back to Windows (approve the permission prompt)...'
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        try { if ((Invoke-Elevated 'Set-BluetoothDriver.ps1' @('-Mode', 'Intel')) -eq 0) { Write-Output '[Q7] Windows Bluetooth restored'; return } }
        catch { Write-Warning "Permission prompt was declined or failed: $_" }
    }
    Write-Warning 'Bluetooth was NOT restored. Run tools\Restore-Bluetooth.ps1 as administrator, or restart Windows (if the boot safety net is installed).'
}

Write-Output '[Q7] Lending the Bluetooth adapter to the bridge (approve the permission prompt)...'
if ((Invoke-Elevated 'Set-BluetoothDriver.ps1' @('-Mode', 'WinUSB')) -ne 0) {
    Get-Content -LiteralPath (Join-Path $run 'driver-switch.log') -Tail 15 -ErrorAction SilentlyContinue
    throw 'Driver switch failed; Windows Bluetooth was left as it was (details above).'
}
try {
    & $probe --hci | Out-Null
    if ($LASTEXITCODE) {
        # The AX201 needs Intel's driver to load its firmware once after power-up.
        Write-Output '[Q7] Loading adapter firmware through the Intel driver, then switching back (one more prompt)...'
        if ((Invoke-Elevated 'Set-BluetoothDriver.ps1' @('-Mode', 'Intel')) -ne 0) { throw 'Firmware bootstrap failed' }
        Start-Sleep -Seconds 2
        if ((Invoke-Elevated 'Set-BluetoothDriver.ps1' @('-Mode', 'WinUSB')) -ne 0) { throw 'Firmware bootstrap failed' }
        & $probe --hci | Out-Null
        if ($LASTEXITCODE) { throw "The adapter does not answer (probe exit $LASTEXITCODE). If it says access denied, the bridge needs admin rights on this PC." }
    }
    $env:Q7_USB_INSTANCE = $fragment
    if ($DebugPacketLog) { $env:Q7_DEBUG_PKLG = '1' } else { Remove-Item Env:Q7_DEBUG_PKLG -ErrorAction SilentlyContinue }
    Push-Location $run
    $savedCtrlC = [Console]::TreatControlCAsInput
    [Console]::TreatControlCAsInput = $true # Ctrl+C reaches the bridge as "quit" instead of killing this script
    try { & $exe } finally { [Console]::TreatControlCAsInput = $savedCtrlC; Pop-Location }
    Write-Output "[Q7] Bridge exited (code $LASTEXITCODE)"
} finally {
    Remove-Item Env:Q7_USB_INSTANCE -ErrorAction SilentlyContinue
    Restore-WindowsBluetooth
}
