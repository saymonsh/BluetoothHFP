# Gives q7_winstack.exe a package identity so Windows lets it open the phone's hands-free link.
# Needs Developer Mode (Settings > System > For developers). Run as a NORMAL user after Build-Q7.ps1.
# Afterwards run it as "q7-winstack" (the alias carries the identity), e.g.: q7-winstack connect <address>
# Remove with: -Unregister
[CmdletBinding()]
param([switch]$Unregister)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$exe = Join-Path $repo 'build\winstack\Release\q7_winstack.exe'
$layout = Join-Path $repo 'build\winstack-pkg'
$manifest = Join-Path $PSScriptRoot 'AppxManifest.xml'
$registered = Get-AppxPackage -Name 'Q7Bridge.WinStack'
# Same manifest: only swap the exe in the registered folder. Re-registering drops the phone's call
# transport registration and makes Windows ask for access again on the next call.
if (-not $Unregister -and $registered -and (Test-Path -LiteralPath $exe) -and
    (Get-FileHash $manifest).Hash -eq (Get-FileHash (Join-Path $layout 'AppxManifest.xml') -ErrorAction SilentlyContinue).Hash) {
    Copy-Item -LiteralPath $exe -Destination $layout -Force # fails while q7-winstack runs: close it first
    Write-Output '[PKG] Updated the exe in place (registration and Windows permissions kept)'
    return
}
$registered | Remove-AppxPackage
if ($Unregister) { Write-Output '[PKG] Removed'; return }
if (-not (Test-Path -LiteralPath $exe)) { throw "Missing $exe. Run Build-Q7.ps1 first." }
New-Item -ItemType Directory -Force $layout | Out-Null
Copy-Item -LiteralPath $exe, (Join-Path $PSScriptRoot 'AppxManifest.xml') -Destination $layout -Force
# ponytail: 1x1 placeholder logo; the package is never shown in Start (AppListEntry="none").
[IO.File]::WriteAllBytes((Join-Path $layout 'logo.png'), [Convert]::FromBase64String(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=='))
Add-AppxPackage -Register (Join-Path $layout 'AppxManifest.xml')
Write-Output '[PKG] Registered. Run: q7-winstack list'
