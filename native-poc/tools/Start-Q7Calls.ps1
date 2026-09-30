# One action for option A: while this window is open, every Q7 call that is talking (or dialing)
# moves to the PC and plays in the headset as stereo headphones; your voice goes from the PC's
# default microphone to the phone. Close the window (or Ctrl+C) to stop.
# Steps: headset hands-free service off (Set-HeadsetHandsFree.ps1), phone call link connected,
# then per call: q7-winstack transfer + q7-winstack route --stereo until the call ends.
# Needs the packaged tool (../winstack/Register-WinStack.ps1) and sideband call audio
# (Set-ScoRouting.ps1 -Mode Sideband). Runs as a normal user.
#   Start-Q7Calls.ps1 -Phone F8:ED:AE:14:7B:5D -Headset 88:0E:85:20:16:C9
#   Start-Q7Calls.ps1 ... -Shortcut     also puts a "Q7 calls" shortcut on the desktop that starts this
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Phone, [Parameter(Mandatory)][string]$Headset, [switch]$Shortcut)
$ErrorActionPreference = 'Stop'
$tool = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\q7-winstack.exe'
if (-not (Test-Path -LiteralPath $tool)) { throw "Missing $tool. Run ..\winstack\Register-WinStack.ps1 first." }
function Say([string]$text) { Write-Host "$(Get-Date -Format HH:mm:ss.fff)  $text" }

if ($Shortcut) {
    $link = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Q7 calls.lnk'
    $s = (New-Object -ComObject WScript.Shell).CreateShortcut($link)
    $s.TargetPath = (Get-Command pwsh).Source
    $s.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Phone $Phone -Headset $Headset"
    $s.WorkingDirectory = $PSScriptRoot
    $s.Save()
    Say "Shortcut: $link"
}

# Two windows would each start a router on the same call.
$single = New-Object Threading.Mutex($false, 'Local\Q7Calls')
if (-not $single.WaitOne(0)) { Say 'Already running in another window; closing this one.'; Start-Sleep -Seconds 5; exit 1 }
& (Join-Path $PSScriptRoot 'Set-HeadsetHandsFree.ps1') -Address $Headset -State Off
& $tool connect $Phone | Out-Null
Say 'Ready. Calls on the Q7 will play in the headset. Close this window to stop.'

$route = $null
$first = $false
try {
    while ($true) {
        # Waiting only: transfer exits 0 once a call's audio is on the PC. It is NOT repeated during a
        # call (each run cut the call audio for ~1 s); route --follow keeps the call on the PC and
        # exits when the call ends.
        $said = & $tool transfer $Phone
        # Timing log: the first poll that sees a call (ringing, dialing...), then the move.
        # Windows keeps an ended call listed for a while: status 0 (lost) and 5 (ended) are not new calls.
        $seen = $said | Select-String -Pattern 'Call: status [1-4]\b' | Select-Object -First 1
        if ($seen -and -not $first) { $first = $true; Say "Call seen ($($seen.Matches[0].Value); 1 incoming, 2 dialing, 3 talking, 4 held)" }
        if ($LASTEXITCODE -eq 0) {
            $first = $false
            Say "Call on the PC ($(($said | Select-String 'moved|Already') -replace '^\[TRANSFER\] ', '')): routing to the headset"
            $route = Start-Process -FilePath $tool -ArgumentList 'route', '--phone', $Phone, '--headset', $Headset, '--stereo', '--follow' `
                                  -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $env:TEMP 'q7-route.log')
            $route.WaitForExit()
            $route = $null
            Say 'Call ended. Waiting for the next one.'
            Get-Content (Join-Path $env:TEMP 'q7-route.log') | Select-String '^\[(TIME|CALL)\]|started' | ForEach-Object { Write-Host "            $($_.Line)" }
        }
        Start-Sleep -Milliseconds 500 # transfer itself takes ~0.3 s, so a new call is seen within ~1 s
    }
} finally {
    if ($route -and -not $route.HasExited) { Stop-Process -Id $route.Id -Force }
}
