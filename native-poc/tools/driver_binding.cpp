// Exact-instance driver selection using Microsoft's preinstalled packages.
// Does not create certificates, change hardware IDs, or edit device filters.
// Also shares Bluetooth link keys between Windows and the Q7 bridge (export-keys / import-keys),
// so a device paired on one side does not have to be removed and re-paired on the other.
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static int fail(const char* operation) {
    std::cerr << "[DRIVER] " << operation << " failed win32=" << GetLastError() << '\n';
    return 1;
}

// ---- link keys shared with the bridge ---------------------------------------------------
// Windows keeps BR/EDR link keys as REG_BINARY(16) values named by the device address (12 hex
// digits) under BTHPORT\Parameters\Keys\<adapter address>; that key is readable by SYSTEM only.
// LE keys are subkeys and other values (e.g. CentralIRK) have other names: both are ignored.
// The 16 bytes are in HCI byte order, the same order BTstack stores and sends (see q7_bridge.c).
static const std::wstring bthport = L"SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\";

static int hex(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
static bool address_name(const wchar_t* name) { // "48684a91d560"
    size_t length = 0;
    for (; name[length]; ++length) if (length == 12 || hex(name[length]) < 0) return false;
    return length == 12;
}

// Class of device of a phone (major class Phone) or a headset (Audio/Video: wearable headset,
// hands-free, headphones), the only devices the bridge pairs; same masks as q7_bridge.c.
static bool phone_or_headset(DWORD cod) {
    const DWORD major = cod >> 8 & 0x1f, minor = cod >> 2 & 0x3f;
    return major == 0x02 || (major == 0x04 && (minor == 0x01 || minor == 0x02 || minor == 0x06));
}
static DWORD device_class(const std::wstring& name) { // 0 if Windows has none
    DWORD cod = 0, size = sizeof(cod);
    return RegGetValueW(HKEY_LOCAL_MACHINE, (bthport + L"Devices\\" + name).c_str(), L"COD", RRF_RT_REG_DWORD,
        nullptr, &cod, &size) == ERROR_SUCCESS ? cod : 0;
}

struct BridgeKey { std::string address; std::wstring name; BYTE key[16], base[16]; };
static bool hex_key(const std::string& line, size_t at, BYTE* key) {
    BYTE any = 0;
    for (size_t i = 0; i < 16; ++i) {
        const int high = hex(line[at + 2 * i]), low = hex(line[at + 2 * i + 1]);
        if (high < 0 || low < 0) return false;
        any |= key[i] = static_cast<BYTE>(high << 4 | low); // copied in file order, never reversed
    }
    return any != 0; // an all-zero key is never valid (CVE-2020-26555)
}
// bridge-new-keys.txt is written by the (non-elevated) bridge and read here elevated, so it is
// parsed strictly: only "AA:BB:CC:DD:EE:FF <new key> <key it replaced>" lines (32 hex digits per key),
// at most 32; anything else rejects the whole file. Lines are applied in order (the bridge appends).
static bool parse_bridge_keys(const std::string& text, std::vector<BridgeKey>& out) {
    out.clear();
    if (text.size() > 4096) return false;
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() != 17 + 1 + 32 + 1 + 32 || line[17] != ' ' || line[50] != ' ' || out.size() == 32) return false;
        BridgeKey entry{};
        entry.address = line.substr(0, 17);
        for (int i = 0; i < 6; ++i) {
            const int high = hex(line[i * 3]), low = hex(line[i * 3 + 1]);
            if (high < 0 || low < 0 || (i < 5 && line[i * 3 + 2] != ':')) return false;
            entry.name += L"0123456789abcdef"[high]; entry.name += L"0123456789abcdef"[low];
        }
        if (!hex_key(line, 18, entry.key) || !hex_key(line, 51, entry.base)) return false;
        out.push_back(entry);
    }
    return true;
}

static bool enable_privilege(const wchar_t* name) {
    HANDLE token = nullptr;
    TOKEN_PRIVILEGES privileges{}; privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    // AdjustTokenPrivileges "succeeds" with ERROR_NOT_ALL_ASSIGNED when the token lacks the privilege.
    const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token) &&
        LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid) &&
        AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    if (token) CloseHandle(token);
    return ok;
}
// Backup/restore semantics bypass the SYSTEM-only ACL (needs the privileges enabled above).
static LSTATUS open_keys(const std::wstring& adapter, REGSAM access, HKEY* key) {
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, (bthport + L"Keys\\" + adapter).c_str(), REG_OPTION_BACKUP_RESTORE, access, key);
}

// Windows -> bridge. One line per phone or headset paired over BR/EDR: "AA:BB:CC:DD:EE:FF <key hex>
// <class hex> <name>". Other pairings (keyboards, mice...) never leave the SYSTEM-only registry key.
// Written into the bridge's private run folder, whose ACL (user, SYSTEM, Administrators) it inherits.
static int export_keys(const std::wstring& adapter, const std::wstring& output) {
    if (!enable_privilege(SE_BACKUP_NAME)) return fail("Enable backup privilege");
    std::string lines;
    unsigned count = 0;
    HKEY keys = nullptr;
    LSTATUS status = open_keys(adapter, KEY_READ, &keys);
    if (status == ERROR_SUCCESS) {
        for (DWORD index = 0;; ++index) {
            wchar_t name[32]; BYTE key[16]; DWORD name_size = 32, key_size = sizeof(key), type = 0;
            status = RegEnumValueW(keys, index, name, &name_size, nullptr, &type, key, &key_size);
            if (status == ERROR_MORE_DATA) continue; // longer name or data: not a link key
            if (status != ERROR_SUCCESS) break;
            if (type != REG_BINARY || key_size != 16 || !address_name(name)) continue;
            const DWORD cod = device_class(name);
            if (!phone_or_headset(cod)) continue;
            const std::wstring device = bthport + L"Devices\\" + name;
            char remote[249] = {}; DWORD remote_size = sizeof(remote) - 1; // UTF-8, only used for the log
            if (RegGetValueW(HKEY_LOCAL_MACHINE, device.c_str(), L"Name", RRF_RT_REG_BINARY, nullptr, remote, &remote_size) != ERROR_SUCCESS) remote[0] = 0;
            std::string printable;
            for (const char* c = remote; *c && printable.size() < 48; ++c) printable += (*c >= 0x20 && *c < 0x7f) ? *c : '?';
            char line[160];
            int used = sprintf_s(line, "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c ", name[0], name[1], name[2], name[3], name[4], name[5],
                name[6], name[7], name[8], name[9], name[10], name[11]);
            for (int i = 0; i < 16; ++i) used += sprintf_s(line + used, sizeof(line) - used, "%02x", key[i]);
            sprintf_s(line + used, sizeof(line) - used, " %06lx %s\n", cod & 0xffffff, printable.empty() ? "(no name)" : printable.c_str());
            lines += line;
            ++count;
        }
        RegCloseKey(keys);
        if (status != ERROR_NO_MORE_ITEMS) { SetLastError(status); return fail("Read Windows link keys"); }
    } else if (status != ERROR_FILE_NOT_FOUND) { SetLastError(status); return fail("Open Windows link keys"); }
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!(file << lines).flush()) { std::cerr << "[KEYS] Cannot write the key file\n"; return 1; }
    std::cout << "[KEYS] Shared " << count << " Windows pairing(s) with the bridge\n";
    return 0;
}

// Bridge -> Windows: keys the bridge created by (re-)pairing its phone or headset. A key is written only
// where Windows still holds the key the bridge replaced (compare-and-swap), so a pairing Windows made in
// the meantime is newer and wins, and a device Windows has not paired never gets a Windows pairing.
// Only devices Windows itself lists as a phone or headset are touched, whatever the file says.
static int import_keys(const std::wstring& adapter, const std::wstring& input) {
    std::ifstream file(input, std::ios::binary);
    if (!file) { std::cerr << "[KEYS] Cannot read the bridge key file\n"; return 1; }
    std::string text(4097, '\0');
    file.read(&text[0], static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(file.gcount()));
    std::vector<BridgeKey> entries;
    if (!parse_bridge_keys(text, entries)) { std::cerr << "[KEYS] Rejected the bridge key file (unexpected content)\n"; return 65; }
    if (!enable_privilege(SE_BACKUP_NAME) || !enable_privilege(SE_RESTORE_NAME)) return fail("Enable backup/restore privileges");
    HKEY keys = nullptr;
    const LSTATUS status = open_keys(adapter, KEY_READ | KEY_SET_VALUE, &keys);
    if (status == ERROR_FILE_NOT_FOUND) { std::cout << "[KEYS] Windows has no pairings on this adapter; nothing to update\n"; return 0; }
    if (status != ERROR_SUCCESS) { SetLastError(status); return fail("Open Windows link keys"); }
    unsigned updated = 0;
    for (const auto& entry : entries) {
        BYTE current[16]; DWORD type = 0, size = sizeof(current);
        if (RegQueryValueExW(keys, entry.name.c_str(), nullptr, &type, current, &size) != ERROR_SUCCESS ||
            type != REG_BINARY || size != 16 || memcmp(current, entry.key, 16) == 0) continue; // not paired in Windows, or done
        if (!phone_or_headset(device_class(entry.name))) {
            std::cout << "[KEYS] " << entry.address << " is not a phone or headset in Windows; its pairing is left unchanged\n";
            continue;
        }
        if (memcmp(current, entry.base, 16) != 0) {
            std::cout << "[KEYS] " << entry.address << " was paired again in Windows since; Windows' newer pairing is kept\n";
            continue;
        }
        const LSTATUS written = RegSetValueExW(keys, entry.name.c_str(), 0, REG_BINARY, entry.key, 16);
        if (written != ERROR_SUCCESS) { RegCloseKey(keys); SetLastError(written); return fail("Update Windows link key"); }
        std::cout << "[KEYS] Updated the Windows pairing of " << entry.address << " with the bridge's key\n";
        ++updated;
    }
    RegCloseKey(keys);
    std::cout << "[KEYS] Updated " << updated << " Windows pairing(s) with keys from the bridge\n";
    return 0;
}

// Runnable check (ctest): the elevated parser accepts only well-formed bridge key files, and only
// phones and headsets pass the class filter.
static int selftest() {
    std::vector<BridgeKey> keys;
    const std::string good = "F8:ED:AE:14:7B:5D 000102030405060708090a0b0c0d0e0f 0f0e0d0c0b0a09080706050403020100\r\n"
                             "88:0e:85:20:16:c9 FFEEDDCCBBAA99887766554433221100 00112233445566778899aabbccddeeff\n";
    if (!parse_bridge_keys(good, keys) || keys.size() != 2 || keys[0].name != L"f8edae147b5d" || keys[1].name != L"880e852016c9" ||
        keys[0].address != "F8:ED:AE:14:7B:5D" || keys[0].key[0] != 0x00 || keys[0].key[15] != 0x0f || keys[1].key[0] != 0xff ||
        keys[0].base[0] != 0x0f || keys[1].base[15] != 0xff) return 1; // byte order kept
    if (!parse_bridge_keys("", keys) || !keys.empty()) return 2;
    const char* bad[] = {
        "F8:ED:AE:14:7B:5D 000102030405060708090a0b0c0d0e 0f0e0d0c0b0a09080706050403020100\n",     // short key
        "F8:ED:AE:14:7B:5D 000102030405060708090a0b0c0d0e0f0 f0e0d0c0b0a09080706050403020100\n",   // shifted field
        "F8-ED-AE-14-7B-5D 000102030405060708090a0b0c0d0e0f 0f0e0d0c0b0a09080706050403020100\n",   // wrong separator
        "F8:ED:AE:14:7B:5D 0001020304050607080g0a0b0c0d0e0f 0f0e0d0c0b0a09080706050403020100\n",   // not hex
        "F8:ED:AE:14:7B:5D 00000000000000000000000000000000 0f0e0d0c0b0a09080706050403020100\n",   // null key
        "F8:ED:AE:14:7B:5D 000102030405060708090a0b0c0d0e0f 00000000000000000000000000000000\n",   // no replaced key
        "F8:ED:AE:14:7B:5D 000102030405060708090a0b0c0d0e0f\n",                                    // replaced key missing
        "F8:ED:AE:14:7B:5D 000102030405060708090a0b0c0d0e0f 0f0e0d0c0b0a09080706050403020100\n\n", // blank line
        "F8:ED:AE:14:7B:5D 000102030405060708090a0b0c0d0e0f 0f0e0d0c0b0a09080706050403020100 x\n", // extra field
    };
    for (const char* text : bad) if (parse_bridge_keys(text, keys)) return 3;
    std::string many;
    for (int i = 0; i < 33; ++i) many += "F8:ED:AE:14:7B:5D 000102030405060708090a0b0c0d0e0f 0f0e0d0c0b0a09080706050403020100\n";
    if (parse_bridge_keys(many, keys)) return 4;
    if (!address_name(L"48684a91d560") || address_name(L"48684a91d56") || address_name(L"48684a91d5600") ||
        address_name(L"48684a91d56g") || address_name(L"CentralIRK")) return 5;
    // Q7 (phone), soundcore P40i (wearable headset); a keyboard and a speaker are never touched.
    if (!phone_or_headset(0x5A020C) || !phone_or_headset(0x248404) || phone_or_headset(0x002540) ||
        phone_or_headset(0x240414) || phone_or_headset(0)) return 6;
    std::cout << "ax201_driver selftest: ok\n";
    return 0;
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
    if (argc == 2 && std::wstring(argv[1]) == L"selftest") return selftest();
    if (argc == 4 && (std::wstring(argv[1]) == L"export-keys" || std::wstring(argv[1]) == L"import-keys")) {
        if (!address_name(argv[2])) return 64;
        return std::wstring(argv[1]) == L"export-keys" ? export_keys(argv[2], argv[3]) : import_keys(argv[2], argv[3]);
    }
    if (argc == 3 && std::wstring(argv[1]) == L"veto") {
        const std::wstring instance = argv[2];
        if (instance.find(L"USB\\VID_8087&PID_0026\\") != 0) return 64;
        return veto(instance);
    }
    if (argc != 5 || (std::wstring(argv[1]) != L"list" && std::wstring(argv[1]) != L"install")) {
        std::cerr << "Usage: ax201_driver list|install <exact-instance> <preinstalled-INF> <section>\n"
                     "       ax201_driver veto <exact-instance>\n"
                     "       ax201_driver export-keys|import-keys <adapter-address-hex> <key-file>\n";
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
