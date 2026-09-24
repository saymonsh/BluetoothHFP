# Builds the Q7 bridge, the AX201 probe and the driver helper. Run as a NORMAL user (no admin).
# Downloads only the needed folders of BTstack at one pinned commit and verifies the commit.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$source = Join-Path $repo 'build\btstack'
$revision = 'e38553977a25fb0b55b383c72c289be0975f422c'

if (-not (Test-Path (Join-Path $source '.git'))) {
    New-Item -ItemType Directory -Force $source | Out-Null
    & git -C $source init -q; & git -C $source remote add origin https://github.com/bluekitchen/btstack.git
    & git -C $source config core.longpaths true
    & git -C $source config core.sparseCheckout true
    "/src/`n/platform/windows/`n/platform/posix/`n/platform/embedded/`n/port/windows-winusb-intel/`n" |
        Set-Content -NoNewline -Encoding ascii (Join-Path $source '.git\info\sparse-checkout')
    & git -C $source fetch -q --depth 1 --filter=blob:none origin $revision
    if ($LASTEXITCODE) { Remove-Item -Recurse -Force $source; throw 'BTstack download failed' }
    & git -C $source checkout -q --detach FETCH_HEAD
    if ($LASTEXITCODE) { Remove-Item -Recurse -Force $source; throw 'BTstack checkout failed' }
}
if ((& git -C $source rev-parse HEAD) -ne $revision) { throw 'Unexpected BTstack revision' }
if (& git -C $source status --porcelain) { throw 'BTstack checkout has local modifications; delete build\btstack and rebuild' }

# Patch the WinUSB transport (written to build\, the checkout stays pristine):
#  1. open only the exact AX201 instance chosen at run time (Q7_USB_INSTANCE, set by Start-Q7.ps1);
#  2. CreateFile failure is INVALID_HANDLE_VALUE, not NULL (and keep cleanup from closing it).
$build = Join-Path $repo 'build\q7'
New-Item -ItemType Directory -Force $build | Out-Null
$transport = Get-Content (Join-Path $source 'platform\windows\hci_transport_h2_winusb.c') -Raw
function Replace-Once([string]$text, [string]$needle, [string]$replacement) {
    $count = ([regex]::Matches($text, [regex]::Escape($needle))).Count
    if ($count -ne 1) { throw "Patch anchor found $count times: $needle" }
    return $text.Replace($needle, $replacement)
}
$transport = Replace-Once $transport 'static int usb_try_open_device(const char * device_path){' @'
#include <stdlib.h>
static int q7_device_path_allowed(const char * device_path){
    /* Only the AX201 instance selected by Start-Q7.ps1 (Q7_USB_INSTANCE), compared case-insensitively. */
    const char * want = getenv("Q7_USB_INSTANCE");
    char path[512], wanted[160];
    if (!want || !*want || !device_path) return 0;
    if (strlen(device_path) >= sizeof(path) || strlen(want) >= sizeof(wanted)) return 0;
    strcpy_s(path, sizeof(path), device_path); strcpy_s(wanted, sizeof(wanted), want);
    _strlwr_s(path, sizeof(path)); _strlwr_s(wanted, sizeof(wanted));
    return strstr(path, wanted) != NULL;
}
static int usb_try_open_device(const char * device_path){
    if (!q7_device_path_allowed(device_path)) return 0;
'@
$transport = Replace-Once $transport 'if (!usb_device_handle) goto exit_on_error;' `
    'if (usb_device_handle == INVALID_HANDLE_VALUE) { usb_device_handle = NULL; goto exit_on_error; }'
$patched = Join-Path $build 'hci_transport_h2_winusb.c'
[IO.File]::WriteAllText($patched, $transport)

# Patch hci.c: when one of two SCO links closes, upstream reads the just-freed connection
# instead of the remaining one, so the remaining call audio gets the wrong USB setting.
$hci = Get-Content (Join-Path $source 'src\hci.c') -Raw
$pattern = '(if \(other_conn == connection\) continue;\s+)if \(connection->address_type != BD_ADDR_TYPE_SCO\) continue;(\s+)int multiplier = hci_sco_get_multiplier_for_voice_setting\(connection->sco_voice_setting\);'
$found = [regex]::Matches($hci, $pattern)
if ($found.Count -ne 1) { throw "hci.c patch anchor found $($found.Count) times" }
$hci = [regex]::Replace($hci, $pattern, '${1}if (other_conn->address_type != BD_ADDR_TYPE_SCO) continue;${2}int multiplier = hci_sco_get_multiplier_for_voice_setting(other_conn->sco_voice_setting);')
$patchedHci = Join-Path $build 'hci.c'
[IO.File]::WriteAllText($patchedHci, $hci)

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -version '[17.0,18.0)' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 C++ build tools are required' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { throw "CMake not found in $vs" }
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'

function Build([string]$sourceDir, [string]$buildDir, [string[]]$extra) {
    & $cmake -Wno-dev -S $sourceDir -B $buildDir -G 'Visual Studio 17 2022' -A x64 @extra
    if ($LASTEXITCODE) { throw "Configure failed: $sourceDir" }
    & $cmake --build $buildDir --config Release --parallel 4
    if ($LASTEXITCODE) { throw "Build failed: $sourceDir" }
    & $ctest --test-dir $buildDir -C Release --output-on-failure
    if ($LASTEXITCODE) { throw "Tests failed: $sourceDir" }
}
Build (Join-Path $repo 'native-poc') (Join-Path $repo 'build\ax201-poc') @()
Build (Join-Path $repo 'native-poc\q7') $build @("-DBTSTACK_ROOT=$($source.Replace('\','/'))", "-DPATCHED_TRANSPORT=$($patched.Replace('\','/'))", "-DPATCHED_HCI=$($patchedHci.Replace('\','/'))")
Write-Output "[BUILD] $build\Release\q7_bridge.exe"
