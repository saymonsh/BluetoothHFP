// Exact-instance driver selection using Microsoft's preinstalled packages.
// Does not create certificates, change hardware IDs, or edit device filters.
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <iostream>
#include <string>
#include <vector>

static int fail(const char* operation) {
    std::cerr << "[DRIVER] " << operation << " failed win32=" << GetLastError() << '\n';
    return 1;
}
// Diagnostic: ask Plug and Play whether the adapter can be removed right now, and if not,
// which device or driver vetoes it. If nothing vetoes, the device is removed and immediately
// re-enumerated, so Windows re-binds its normal (Intel) driver.
static int veto(const std::wstring& instance) {
    DEVINST node = 0, root = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(instance.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return fail("Locate device");
    PNP_VETO_TYPE type = PNP_VetoTypeUnknown;
    wchar_t name[MAX_PATH] = {};
    const CONFIGRET result = CM_Query_And_Remove_SubTreeW(node, &type, name, MAX_PATH, CM_REMOVE_NO_RESTART);
    std::wcout << L"[VETO] result=" << result << L" vetoType=" << type << L" vetoName=" << name << L'\n';
    if (result == CR_SUCCESS && CM_Locate_DevNodeW(&root, nullptr, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
        CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS);
        std::wcout << L"[VETO] Not vetoed; device removed and re-enumerated\n";
    }
    return result == CR_SUCCESS ? 0 : 5;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"veto") {
        const std::wstring instance = argv[2];
        if (instance.find(L"USB\\VID_8087&PID_0026\\") != 0) return 64;
        return veto(instance);
    }
    if (argc != 5 || (std::wstring(argv[1]) != L"list" && std::wstring(argv[1]) != L"install")) {
        std::cerr << "Usage: ax201_driver list|install <exact-instance> <preinstalled-INF> <section>\n";
        return 64;
    }
    const std::wstring instance = argv[2], inf = argv[3], section = argv[4];
    if (instance.find(L"USB\\VID_8087&PID_0026\\") != 0 || inf.size() >= MAX_PATH) return 64;
    const bool install = std::wstring(argv[1]) == L"install";
    // Validate the package and unique section before detaching anything.
    if (install) {
        wchar_t preview[] = L"list";
        wchar_t* preview_args[] = {argv[0], preview, argv[2], argv[3], argv[4]};
        const int checked = wmain(5, preview_args);
        if (checked) return checked;
    }
    HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE) return fail("CreateDeviceInfoList");
    SP_DEVINFO_DATA device{}; device.cbSize = sizeof(device);
    if (!SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0, &device)) {
        int result = fail("OpenDeviceInfo"); SetupDiDestroyDeviceInfoList(set); return result;
    }
    // Preview uses a global list. A real cross-class install first removes the
    // old binding using documented null-driver installation (package retained).
    // The caller must have an exported backup and restore on any later failure.
    if (install) {
        BOOL reboot = FALSE;
        if (!DiInstallDevice(nullptr, set, &device, nullptr, DIIDFLAG_INSTALLNULLDRIVER, &reboot))
            return fail("Detach old binding");
        std::cout << "[DRIVER] Detached exact Bluetooth instance; rebootRequired=" << reboot << '\n';
        SetupDiDestroyDeviceInfoList(set);
        set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
        device = {}; device.cbSize = sizeof(device);
        if (!SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0, &device)) return fail("Reopen detached device");
    }
    SP_DEVINFO_DATA* scope = install ? &device : nullptr;
    SP_DEVINSTALL_PARAMS_W params{}; params.cbSize = sizeof(params);
    if (!SetupDiGetDeviceInstallParamsW(set, scope, &params)) return fail("GetDeviceInstallParams");
    params.Flags |= DI_ENUMSINGLEINF | DI_QUIETINSTALL;
    params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    wcscpy_s(params.DriverPath, inf.c_str());
    if (!SetupDiSetDeviceInstallParamsW(set, scope, &params) ||
        !SetupDiBuildDriverInfoList(set, scope, SPDIT_CLASSDRIVER)) return fail("BuildDriverInfoList");
    SP_DRVINFO_DATA_W selected{}; unsigned matches = 0;
    for (DWORD index = 0;; ++index) {
        SP_DRVINFO_DATA_W driver{}; driver.cbSize = sizeof(driver);
        if (!SetupDiEnumDriverInfoW(set, scope, SPDIT_CLASSDRIVER, index, &driver)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) return fail("EnumDriverInfo");
            break;
        }
        std::vector<BYTE> bytes(sizeof(SP_DRVINFO_DETAIL_DATA_W) + 8192, 0);
        auto detail = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(bytes.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDriverInfoDetailW(set, scope, &driver, detail,
                static_cast<DWORD>(bytes.size()), nullptr)) return fail("GetDriverInfoDetail");
        std::wcout << L"[DRIVER] " << driver.Description << L" provider=" << driver.ProviderName
                   << L" section=" << detail->SectionName << L" INF=" << detail->InfFileName << L'\n';
        if (_wcsicmp(section.c_str(), detail->SectionName) == 0) { selected = driver; ++matches; }
    }
    if (matches != 1) {
        std::cerr << "[DRIVER] Expected one selected section; found " << matches << '\n';
        SetupDiDestroyDeviceInfoList(set); return 2;
    }
    std::wcout << L"[DRIVER] Target=" << instance << L" selected=" << selected.Description << L'\n';
    if (!install) { SetupDiDestroyDeviceInfoList(set); return 0; }
    if (!SetupDiSetSelectedDriverW(set, &device, &selected)) return fail("SetSelectedDriver");
    BOOL reboot = FALSE;
    const BOOL ok = DiInstallDevice(nullptr, set, &device, &selected, 0, &reboot);
    const int result = ok ? (reboot ? 3010 : 0) : fail("DiInstallDevice");
    if (ok) std::cout << "[DRIVER] Installed on exact instance; rebootRequired=" << reboot << '\n';
    SetupDiDestroyDeviceInfoList(set);
    return result;
}
