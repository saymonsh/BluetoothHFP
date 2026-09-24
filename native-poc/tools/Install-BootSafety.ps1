# One-time, optional safety net (run as administrator). At every Windows start, if the
# AX201 was left on WinUSB (power loss, closed window, crash), give it back to Windows.
# Installs: C:\ProgramData\Q7Handsfree\Restore-Bluetooth.ps1 (admin-only writable)
#           scheduled task "Q7 Bluetooth safety net" (SYSTEM, at startup).
# Remove with: -Uninstall
[CmdletBinding()]
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run as administrator' }
$task = 'Q7 Bluetooth safety net'
$target = Join-Path $env:ProgramData 'Q7Handsfree'
if ($Uninstall) {
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction SilentlyContinue
    Write-Output "[SAFETY] Removed task and $target"
    return
}
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$backup = Get-ChildItem (Join-Path $repo '.local\ax201-backup') -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
if (-not $backup) { throw 'Run Backup-Driver.ps1 first' }
$instance = (Get-Content -LiteralPath (Join-Path $backup.FullName 'snapshot.json') -Raw | ConvertFrom-Json).InstanceId
if ($instance -notlike 'USB\VID_8087&PID_0026\*') { throw 'Backup does not describe the AX201' }

# The task runs as SYSTEM, so its script must not be writable by normal users.
New-Item -ItemType Directory -Force $target | Out-Null
& icacls.exe $target /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
if ($LASTEXITCODE) { throw 'Could not secure the ProgramData folder' }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Restore-Bluetooth.ps1') -Destination $target -Force
Set-Content -LiteralPath (Join-Path $target 'instance.txt') -Value $instance -Encoding ascii

$powershell = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$action = New-ScheduledTaskAction -Execute $powershell -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$target\Restore-Bluetooth.ps1`""
$trigger = New-ScheduledTaskTrigger -AtStartup
$principalTask = New-ScheduledTaskPrincipal -UserId 'S-1-5-18' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 5)
Register-ScheduledTask -TaskName $task -Action $action -Trigger $trigger -Principal $principalTask -Settings $settings -Force | Out-Null
Write-Output "[SAFETY] Installed '$task'. It does nothing unless Bluetooth was left on WinUSB. Log: $target\restore.log"
