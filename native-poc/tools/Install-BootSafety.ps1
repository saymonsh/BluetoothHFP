# One-time, optional safety net (run as administrator). At every Windows start, if the
# AX201 was left on WinUSB (power loss, closed window, crash), give it back to Windows.
# Installs: C:\ProgramData\Q7Handsfree\Restore-Bluetooth.ps1 (only admins/SYSTEM can write)
#           scheduled task "Q7 Bluetooth safety net" (SYSTEM, at startup).
# Remove with: -Uninstall
[CmdletBinding()]
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run as administrator' }
$task = 'Q7 Bluetooth safety net'
$target = Join-Path $env:ProgramData 'Q7Handsfree'
# The folder is recreated below; it may hold the record of Bluetooth devices disabled for the bridge.
if (Test-Path -LiteralPath (Join-Path $target 'disabled-devnodes.txt')) {
    throw "Some Bluetooth devices are still disabled for the bridge (recorded in $target). Run native-poc\tools\Restore-Bluetooth.ps1 as administrator first."
}
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
# Never reuse an existing folder: a normal user could have created it first and kept control of it.
if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
if ($Uninstall) { Write-Output "[SAFETY] Removed task and $target"; return }

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$backup = Get-ChildItem (Join-Path $repo '.local\ax201-backup') -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
if (-not $backup) { throw 'Run Backup-Driver.ps1 first' }
$instance = (Get-Content -LiteralPath (Join-Path $backup.FullName 'snapshot.json') -Raw | ConvertFrom-Json).InstanceId
if ($instance -notlike 'USB\VID_8087&PID_0026\*') { throw 'Backup does not describe the AX201' }

# The task runs as SYSTEM, so its folder is created fresh, owned by Administrators, and locked down
# before anything is put in it.
New-Item -ItemType Directory $target | Out-Null
& icacls.exe $target /setowner '*S-1-5-32-544' | Out-Null
if ($LASTEXITCODE) { throw 'Could not set the folder owner' }
& icacls.exe $target /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
if ($LASTEXITCODE) { throw 'Could not secure the ProgramData folder' }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Restore-Bluetooth.ps1') -Destination $target
Set-Content -LiteralPath (Join-Path $target 'instance.txt') -Value $instance -Encoding ascii

$powershell = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$action = New-ScheduledTaskAction -Execute $powershell -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$target\Restore-Bluetooth.ps1`""
$trigger = New-ScheduledTaskTrigger -AtStartup
$principalTask = New-ScheduledTaskPrincipal -UserId 'S-1-5-18' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 5)
Register-ScheduledTask -TaskName $task -Action $action -Trigger $trigger -Principal $principalTask -Settings $settings -Force | Out-Null
Write-Output "[SAFETY] Installed '$task'. It does nothing unless Bluetooth was left on WinUSB. Log: $target\restore.log"
