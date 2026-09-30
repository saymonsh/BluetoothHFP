// Call router and two-link feasibility test on Windows' own Bluetooth stack.
// The AX201 stays on its Windows driver: nothing is swapped and nothing is unpaired.
// The Q7 is paired to Windows as a phone (Windows is its hands-free unit), the headset
// is paired normally, and this tool moves call audio between their Windows endpoints:
//   phone capture   -> headset render  (the other party, to your ears)
//   headset capture -> phone render    (your voice, to the other party)
// The same two streams feed the local call-audio pipe served by ../q7/call_tap.cpp.
//   q7_winstack list
//   q7_winstack connect <phone address> | disconnect <phone address>
//   q7_winstack route [--phone <address|name>] [--headset <address|name>] [--seconds N] [--connect] [--stereo]
//   q7_winstack selftest
// --stereo fits the laptop's one call-audio link: the headset plays the call as stereo
// headphones and your voice comes from the PC's default microphone instead of the headset's.
// It needs the headset's hands-free service off (../tools/Set-HeadsetHandsFree.ps1 -State Off);
// otherwise Windows sends the moved call to the headset's hands-free link and it bounces back.
// route exits 0 once the two-link verdict was reached, 3 if it never was, 2 if it cannot start.
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h> // defines the endpoint property keys here (no import lib carries FormFactor)
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <winrt/Windows.ApplicationModel.Calls.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <mutex>
#include <thread>
#include <vector>
#include "audio/audio_ring_buffer.h"
#include "call_tap.h"

using namespace winrt;
using namespace Windows::ApplicationModel::Calls;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Foundation;

namespace {
constexpr int kPhone = 2, kAudioVideo = 4; // Bluetooth major device classes
// ponytail: 8 kHz mono end to end; the Q7 line is narrowband anyway. WASAPI converts from
// each endpoint's own rate. Upgrade path: open at 16 kHz if a wideband (mSBC) link appears.
constexpr unsigned kRate = 8000;
constexpr int kVerdictSeconds = 10;
const char* const kLabels[4] = {"phone-in", "headset-out", "headset-in", "phone-out"};

struct BtDevice { std::wstring address, name; int major = -1, connected = -1; GUID container{}; };
struct Endpoint { std::wstring id, name; bool capture = false; DWORD state = 0; UINT form = UnknownFormFactor; GUID container{}; };
struct Tick { bool running; uint64_t frames; int peak; };

std::string u8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
    return text;
}
// "F8:ED:AE:14:7B:5D", "f8-ed-..." or "F8EDAE147B5D" -> "F8EDAE147B5D"; empty if not an address.
std::wstring normalize_address(const std::wstring& text) {
    std::wstring hex;
    for (wchar_t c : text) {
        if (c == L':' || c == L'-') continue;
        if (!iswxdigit(c)) return {};
        hex += wchar_t(towupper(c));
    }
    return hex.size() == 12 ? hex : std::wstring{};
}
std::string pretty(const std::wstring& address) {
    if (address.size() != 12) return "??:??:??:??:??:??";
    std::string text;
    for (size_t i = 0; i < 12; i += 2) { if (i) text += ':'; text += char(address[i]); text += char(address[i + 1]); }
    return text;
}
std::string guid_text(const GUID& id) {
    wchar_t text[40] = {};
    StringFromGUID2(id, text, 40);
    return u8(text);
}
bool has_container(const GUID& id) { return id != GUID{}; }
std::string failure(const hresult_error& error) {
    char code[24];
    sprintf_s(code, " (0x%08X)", unsigned(error.code().value));
    return u8(std::wstring(error.message())) + code;
}
const char* yes_no(bool value) { return value ? "yes" : "no"; }
const char* major_name(int major) {
    static const char* const names[] = {"Miscellaneous", "Computer", "Phone", "NetworkAccessPoint", "AudioVideo",
                                        "Peripheral", "Imaging", "Wearable", "Toy", "Health"};
    return major >= 0 && major < 10 ? names[major] : "unknown";
}
const char* state_name(DWORD state) {
    return state == DEVICE_STATE_ACTIVE ? "active" : state == DEVICE_STATE_DISABLED ? "disabled"
         : state == DEVICE_STATE_NOTPRESENT ? "notpresent" : state == DEVICE_STATE_UNPLUGGED ? "unplugged" : "unknown";
}
const char* form_name(UINT form) {
    static const char* const names[] = {"network", "speakers", "line", "headphones", "microphone", "headset",
                                        "handset", "digital", "spdif", "display", "unknown-form"};
    return form < 11 ? names[form] : "unknown-form";
}

// Windows calls can stall; never hang the console on one.
template <typename Operation> auto wait(const Operation& operation) {
    if (operation.wait_for(std::chrono::seconds(20)) == AsyncStatus::Started) {
        operation.Cancel();
        throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    return operation.GetResults();
}

// ---- Selection (pure; covered by selftest) ----

bool has_endpoints(const BtDevice& device, const std::vector<Endpoint>& endpoints) {
    return has_container(device.container) &&
        std::any_of(endpoints.begin(), endpoints.end(), [&](const Endpoint& e) { return e.container == device.container; });
}
// A --phone/--headset value matches a paired device by address or by a case-insensitive
// part of its name; with no value, the only device of that major class that has audio
// endpoints. Anything else is refused and reported, never guessed.
const BtDevice* pick_device(const std::vector<BtDevice>& devices, const std::vector<Endpoint>& endpoints,
                            const std::wstring& wanted, int major, int& matches) {
    const std::wstring address = normalize_address(wanted), fragment = lower(wanted);
    const BtDevice* found = nullptr;
    matches = 0;
    for (const auto& device : devices) {
        const bool match = wanted.empty() ? device.major == major && has_endpoints(device, endpoints)
            : (!address.empty() && device.address == address) || lower(device.name).find(fragment) != std::wstring::npos;
        if (match) { found = &device; ++matches; }
    }
    return matches == 1 ? found : nullptr;
}
// Lower is better: an endpoint Windows no longer has (notpresent/disabled) last; otherwise the
// call (hands-free) form factors over a stereo-only one, and only then live over unplugged. Form
// comes first because a headset's hands-free endpoint can still be unplugged while its stereo one
// is active, and the stereo one goes silent once the call link opens; the 2 s retry picks the
// hands-free one up when it goes live. Names are never used: Windows localizes them.
int rank(const Endpoint& e) {
    const bool gone = e.state == DEVICE_STATE_NOTPRESENT || e.state == DEVICE_STATE_DISABLED;
    const int form = e.form == Headset || e.form == Handset ? 0 : e.form == UnknownFormFactor ? 1 : 2;
    return gone * 6 + form * 2 + (e.state != DEVICE_STATE_ACTIVE);
}
// Best endpoint of one direction in a device's container; null when there is none or when
// two rank equally (the caller prints the candidates instead of taking the first).
const Endpoint* pick_endpoint(const std::vector<Endpoint>& endpoints, const GUID& container, bool capture) {
    const Endpoint* best = nullptr;
    bool tie = false;
    for (const auto& e : endpoints) {
        if (e.capture != capture || !has_container(container) || e.container != container) continue;
        if (!best || rank(e) < rank(*best)) { best = &e; tie = false; }
        else if (rank(e) == rank(*best)) tie = true;
    }
    return tie ? nullptr : best;
}
// The headset's one stereo (music) output that Windows still has; null when none or several.
const Endpoint* pick_stereo(const std::vector<Endpoint>& endpoints, const GUID& container) {
    const Endpoint* best = nullptr;
    int count = 0;
    for (const auto& e : endpoints)
        if (!e.capture && has_container(container) && e.container == container && e.form != Headset && e.form != Handset &&
            e.state != DEVICE_STATE_NOTPRESENT && e.state != DEVICE_STATE_DISABLED) { best = &e; ++count; }
    return count == 1 ? best : nullptr;
}
// Route order: phone-in feeds headset-out, headset-in feeds phone-out.
// mic (--stereo): the headset stays on its stereo link and the voice comes from this PC
// microphone, so only the phone holds a call-audio link. Null mic = the headset's hands-free pair.
// A mic inside the phone or headset is refused: it would open a second call-audio link.
std::array<const Endpoint*, 4> pick_streams(const std::vector<Endpoint>& endpoints, const BtDevice& phone, const BtDevice& headset,
                                            const std::wstring* mic = nullptr) {
    const Endpoint* voice = nullptr;
    if (mic)
        for (const auto& e : endpoints)
            if (e.capture && _wcsicmp(e.id.c_str(), mic->c_str()) == 0 && e.container != phone.container && e.container != headset.container) voice = &e;
    return {pick_endpoint(endpoints, phone.container, true),
            mic ? pick_stereo(endpoints, headset.container) : pick_endpoint(endpoints, headset.container, false),
            mic ? voice : pick_endpoint(endpoints, headset.container, true), pick_endpoint(endpoints, phone.container, false)};
}
// Seconds in a row during which all four streams ran, moved frames and carried sound.
int next_streak(int streak, const std::array<Tick, 4>& ticks) {
    for (const auto& t : ticks) if (!t.running || !t.frames || !t.peak) return 0;
    return streak + 1;
}

// ---- Windows enumeration ----

std::vector<BtDevice> paired_devices() {
    std::vector<BtDevice> devices;
    const auto properties = single_threaded_vector<hstring>({L"System.Devices.Aep.DeviceAddress", L"System.Devices.Aep.ContainerId"});
    const auto infos = wait(DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromPairingState(true),
                                                            properties, DeviceInformationKind::AssociationEndpoint));
    for (const auto& info : infos) {
        BtDevice device;
        device.name = info.Name();
        device.address = normalize_address(std::wstring(unbox_value_or<hstring>(info.Properties().TryLookup(L"System.Devices.Aep.DeviceAddress"), L"")));
        device.container = unbox_value_or<guid>(info.Properties().TryLookup(L"System.Devices.Aep.ContainerId"), guid{});
        try {
            if (auto bluetooth = wait(BluetoothDevice::FromIdAsync(info.Id()))) {
                wchar_t hex[16];
                swprintf_s(hex, L"%012llX", static_cast<unsigned long long>(bluetooth.BluetoothAddress()));
                if (device.address.empty()) device.address = hex;
                device.major = int(bluetooth.ClassOfDevice().MajorClass());
                device.connected = bluetooth.ConnectionStatus() == BluetoothConnectionStatus::Connected;
                bluetooth.Close();
            }
        } catch (const hresult_error&) {
            // Class and connection stay "unknown"; the container still matches endpoints.
        }
        devices.push_back(device);
    }
    return devices;
}

std::vector<Endpoint> audio_endpoints() {
    std::vector<Endpoint> endpoints;
    const auto enumerator = create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
    com_ptr<IMMDeviceCollection> collection;
    check_hresult(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, collection.put()));
    UINT count = 0;
    check_hresult(collection->GetCount(&count));
    std::vector<com_ptr<IMMDevice>> devices;
    for (UINT i = 0; i < count; ++i) {
        com_ptr<IMMDevice> device;
        if (SUCCEEDED(collection->Item(i, device.put()))) devices.push_back(device);
    }
    // Seen 2026-09-29: the phone's hands-free call endpoints ("Q7 Hands-Free HF Audio") are active and
    // open fine, but EnumAudioEndpoints leaves them out. Plug and Play lists every present endpoint as
    // SWD\MMDEVAPI\<endpoint id>, so add those it missed.
    ULONG size = 0;
    if (CM_Get_Device_ID_List_SizeW(&size, L"SWD\\MMDEVAPI", CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) == CR_SUCCESS) {
        std::vector<wchar_t> ids(size);
        if (CM_Get_Device_ID_ListW(L"SWD\\MMDEVAPI", ids.data(), size, CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) == CR_SUCCESS)
            for (const wchar_t* p = ids.data(); *p; p += wcslen(p) + 1) {
                const std::wstring instance = p, prefix = L"SWD\\MMDEVAPI\\";
                if (instance.compare(0, prefix.size(), prefix) != 0) continue;
                com_ptr<IMMDevice> device;
                if (FAILED(enumerator->GetDevice(instance.substr(prefix.size()).c_str(), device.put()))) continue;
                LPWSTR id = nullptr;
                if (FAILED(device->GetId(&id))) continue;
                const bool known = std::any_of(devices.begin(), devices.end(), [&](const com_ptr<IMMDevice>& d) {
                    LPWSTR other = nullptr; bool same = SUCCEEDED(d->GetId(&other)) && _wcsicmp(other, id) == 0; CoTaskMemFree(other); return same; });
                CoTaskMemFree(id);
                if (!known) devices.push_back(device);
            }
    }
    for (const auto& device : devices) {
        Endpoint e;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) continue;
        e.id = id;
        CoTaskMemFree(id);
        device->GetState(&e.state);
        EDataFlow flow = eRender;
        if (const auto endpoint = device.try_as<IMMEndpoint>()) endpoint->GetDataFlow(&flow);
        e.capture = flow == eCapture;
        com_ptr<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, store.put()))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) e.name = value.pwszVal;
            PropVariantClear(&value);
            if (SUCCEEDED(store->GetValue(PKEY_Device_ContainerId, &value)) && value.vt == VT_CLSID) e.container = *value.puuid;
            PropVariantClear(&value);
            if (SUCCEEDED(store->GetValue(PKEY_AudioEndpoint_FormFactor, &value)) && value.vt == VT_UI4) e.form = value.ulVal;
            PropVariantClear(&value);
        }
        endpoints.push_back(e);
    }
    return endpoints;
}

// Match a stable address or container, never a friendly name or the first item (two
// paired phones can share a name); same rule as windows/runner/phone_transport.cpp.
bool belongs(const DeviceInformation& info, const BtDevice& device) {
    std::wstring id(info.Id());
    std::transform(id.begin(), id.end(), id.begin(), [](wchar_t c) { return wchar_t(towupper(c)); });
    if (!device.address.empty() && id.find(device.address) != std::wstring::npos) return true;
    const GUID container = unbox_value_or<guid>(info.Properties().TryLookup(L"System.Devices.ContainerId"), guid{});
    return has_container(device.container) && container == device.container;
}
DeviceInformationCollection phone_transports() {
    return wait(DeviceInformation::FindAllAsync(PhoneLineTransportDevice::GetDeviceSelector(PhoneLineTransport::Bluetooth),
                                                single_threaded_vector<hstring>({L"System.Devices.ContainerId"})));
}
const char* access_name(DeviceAccessStatus status) {
    return status == DeviceAccessStatus::Allowed ? "Allowed" : status == DeviceAccessStatus::DeniedByUser ? "DeniedByUser"
         : status == DeviceAccessStatus::DeniedBySystem ? "DeniedBySystem" : "Unspecified";
}
const char* routing_name(TransportDeviceAudioRoutingStatus status) {
    return status == TransportDeviceAudioRoutingStatus::CanRouteToLocalDevice ? "CanRouteToLocalDevice"
         : status == TransportDeviceAudioRoutingStatus::CannotRouteToLocalDevice ? "CannotRouteToLocalDevice" : "Unknown";
}
// The AX201's call-audio (SCO) settings, read-only; tools/Set-ScoRouting.ps1 explains and switches
// them. Sideband allows ONE call-audio link by design, so a two-link result only means something
// next to these values.
std::string sco_routing() {
    static const wchar_t prefix[] = L"USB\\VID_8087&PID_0026\\"; // the AX201, as in Set-ScoRouting.ps1
    static const wchar_t* const names[] = {L"Sco Support Type", L"SCO Max Channels", L"HfpOffloadDisable"};
    static const char* const modes[] = {" (Intel's default, assumed in-band)", " (in-band over HCI)", " (sideband: ONE call-audio link by design)"};
    const ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    ULONG size = 0;
    if (CM_Get_Device_ID_List_SizeW(&size, L"USB", flags) != CR_SUCCESS || !size) return "unknown (cannot list USB devices)";
    std::vector<wchar_t> ids(size);
    if (CM_Get_Device_ID_ListW(L"USB", ids.data(), size, flags) != CR_SUCCESS) return "unknown (cannot list USB devices)";
    std::string text;
    for (const wchar_t* id = ids.data(); *id; id += wcslen(id) + 1) {
        if (_wcsnicmp(id, prefix, std::size(prefix) - 1)) continue;
        const std::wstring key = std::wstring(L"SYSTEM\\CurrentControlSet\\Enum\\") + id + L"\\Device Parameters";
        if (!text.empty()) text += "; ";
        for (size_t i = 0; i < std::size(names); ++i) {
            DWORD value = 0, bytes = sizeof(value);
            const bool set = RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), names[i], RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS;
            text += (i ? ", " : "") + u8(names[i]) + '=' + (set ? std::to_string(value) : std::string("not set"));
            if (!i && set && value < std::size(modes)) text += modes[value];
        }
    }
    return text.empty() ? "unknown (no AX201 radio present)" : text;
}

// ---- list ----

void print_endpoint(const Endpoint& e, const char* note) {
    printf("    %-7s %-10s %-12s %s%s\n      container=%s id=%s\n", e.capture ? "capture" : "render", state_name(e.state),
           form_name(e.form), u8(e.name).c_str(), note, guid_text(e.container).c_str(), u8(e.id).c_str());
}
// Why route cannot pick a role by itself, and what helps: a flag only helps when several qualify.
void explain(const char* prefix, const char* role, const char* flag, int count) {
    if (count) { printf("%s%d paired %s-class devices have audio endpoints; pass %s <address|name>.\n", prefix, count, role, flag); return; }
    printf("%sNo paired %s has audio endpoints yet: pair it, or turn it on so it connects, in Settings > Bluetooth & devices.\n", prefix, role);
    if (!strcmp(role, "phone"))
        printf("%sIf the phone is paired, its call audio may appear only once connected: route --phone <address> --connect\n", prefix);
}

int list() {
    const auto devices = paired_devices();
    const auto endpoints = audio_endpoints();
    printf("Paired Bluetooth Classic devices: %zu\n", devices.size());
    for (const auto& d : devices)
        printf("  %s  %s  class=%s connected=%s container=%s\n", pretty(d.address).c_str(), u8(d.name).c_str(),
               major_name(d.major), d.connected < 0 ? "?" : yes_no(d.connected == 1), guid_text(d.container).c_str());

    printf("\nPhone call transports (Bluetooth):\n");
    try {
        const auto transports = phone_transports();
        if (!transports.Size()) printf("  none (a phone paired to Windows appears here)\n");
        for (const auto& info : transports) {
            printf("  %s", u8(std::wstring(info.Name())).c_str());
            for (const auto& d : devices) if (belongs(info, d)) printf(" [phone %s]", pretty(d.address).c_str());
            try {
                const auto transport = PhoneLineTransportDevice::FromId(info.Id());
                printf(" registered=%s routing=%s", yes_no(transport.IsRegistered()), routing_name(transport.AudioRoutingStatus()));
            } catch (const hresult_error& e) { printf(" (details unavailable: %s)", failure(e).c_str()); }
            printf("\n    id=%s\n", u8(std::wstring(info.Id())).c_str());
        }
    } catch (const hresult_error& e) { printf("  enumeration failed: %s\n", failure(e).c_str()); }

    // Mark what route would pick with no flags.
    int phones = 0, headsets = 0;
    const BtDevice* phone = pick_device(devices, endpoints, L"", kPhone, phones);
    const BtDevice* headset = pick_device(devices, endpoints, L"", kAudioVideo, headsets);
    std::array<const Endpoint*, 4> picks{};
    if (phone && headset) picks = pick_streams(endpoints, *phone, *headset);
    auto note = [&](const Endpoint& e) {
        for (size_t i = 0; i < picks.size(); ++i) if (picks[i] == &e) return std::string("   <= route ") + kLabels[i];
        return std::string();
    };
    printf("\nAudio endpoints: %zu (render and capture, every state)\n", endpoints.size());
    for (const auto& d : devices) {
        printf("  %s %s (%s)%s:\n", d.major == kPhone ? "PHONE" : d.major == kAudioVideo ? "HEADSET" : "OTHER Bluetooth device",
               u8(d.name).c_str(), pretty(d.address).c_str(), d.major == kPhone ? ", its hands-free call audio" : "");
        bool any = false;
        for (const auto& e : endpoints)
            if (has_container(d.container) && e.container == d.container) { print_endpoint(e, note(e).c_str()); any = true; }
        if (!any) printf("    (no audio endpoints)\n");
    }
    printf("  Not a paired Bluetooth device:\n");
    for (const auto& e : endpoints)
        if (std::none_of(devices.begin(), devices.end(), [&](const BtDevice& d) { return has_container(d.container) && e.container == d.container; }))
            print_endpoint(e, "");

    if (!phone || !headset) {
        printf("\nroute cannot choose by itself (it needs exactly one phone and one headset with audio endpoints):\n");
        if (!phone) explain("  ", "phone", "--phone", phones);
        if (!headset) explain("  ", "headset", "--headset", headsets);
    } else if (std::find(picks.begin(), picks.end(), nullptr) != picks.end())
        printf("\nroute found %s and %s but not one clear endpoint for every direction (see the endpoints above).\n", u8(phone->name).c_str(), u8(headset->name).c_str());
    else
        printf("\nroute with no flags: phone %s (%s), headset %s (%s).\n", u8(phone->name).c_str(), pretty(phone->address).c_str(),
               u8(headset->name).c_str(), pretty(headset->address).c_str());
    return 0;
}

// ---- connect / disconnect ----

// This phone's call transport; null (with the reason printed) when Windows has none for it.
PhoneLineTransportDevice find_transport(const BtDevice& phone, const char* tag) {
    DeviceInformation info{nullptr};
    for (const auto& candidate : phone_transports()) if (belongs(candidate, phone)) { info = candidate; break; }
    if (!info) {
        printf("%s Windows exposes no phone call transport for %s. Pair the phone to Windows (Settings > Bluetooth & devices), then run list.\n",
               tag, pretty(phone.address).c_str());
        return nullptr;
    }
    printf("%s Transport: %s\n  id=%s\n", tag, u8(std::wstring(info.Name())).c_str(), u8(std::wstring(info.Id())).c_str());
    try {
        if (auto transport = PhoneLineTransportDevice::FromId(info.Id())) return transport;
        printf("%s FromId returned nothing\n", tag);
    } catch (const hresult_error& e) { printf("%s FromId failed: %s\n", tag, failure(e).c_str()); }
    return nullptr;
}

// RequestAccessAsync, RegisterApp if not registered, ConnectAsync. Each step runs even if an
// earlier one was refused, so every result is on screen. registered_here: this call registered it.
bool connect_steps(const PhoneLineTransportDevice& transport, const char* tag, bool& registered_here) {
    try {
        const auto access = wait(transport.RequestAccessAsync());
        printf("%s RequestAccessAsync: %s (%d)\n", tag, access_name(access), int(access));
        if (access != DeviceAccessStatus::Allowed)
            printf("%s Windows did not allow this unpackaged exe to manage phone calls (that normally needs a packaged app "
                   "with the phoneLineTransportManagement capability). Continuing so each step shows its own result.\n", tag);
    } catch (const hresult_error& e) { printf("%s RequestAccessAsync failed: %s\n", tag, failure(e).c_str()); }
    try {
        const bool registered = transport.IsRegistered();
        printf("%s IsRegistered: %s\n", tag, yes_no(registered));
        if (!registered) {
            transport.RegisterApp();
            registered_here = transport.IsRegistered();
            printf("%s RegisterApp: done; IsRegistered now: %s\n", tag, yes_no(registered_here));
        }
    } catch (const hresult_error& e) {
        printf("%s RegisterApp failed: %s\n", tag, failure(e).c_str());
        // Seen 2026-09-29 on this PC: after this refusal ConnectAsync crashed the process (0xC0000005).
        printf("%s ConnectAsync skipped: Windows refused the registration it needs\n", tag);
        return false;
    }
    // A RegisterApp can leave IsRegistered false (experimental BypassRegistration); ConnectAsync reports the truth.
    bool connected = false;
    try {
        connected = wait(transport.ConnectAsync());
        printf("%s ConnectAsync: %s\n", tag, connected ? "true" : "false");
    } catch (const hresult_error& e) { printf("%s ConnectAsync failed: %s\n", tag, failure(e).c_str()); }
    try { printf("%s AudioRoutingStatus: %s\n", tag, routing_name(transport.AudioRoutingStatus())); }
    catch (const hresult_error& e) { printf("%s AudioRoutingStatus failed: %s\n", tag, failure(e).c_str()); }
    return connected;
}

int connect(const std::wstring& text, bool undo) {
    const char* tag = undo ? "[DISCONNECT]" : "[CONNECT]";
    BtDevice phone;
    phone.address = normalize_address(text);
    if (phone.address.empty()) { printf("%s '%s' is not a Bluetooth address (example F8:ED:AE:14:7B:5D)\n", tag, u8(text).c_str()); return 2; }
    for (const auto& d : paired_devices()) if (d.address == phone.address) phone = d;
    if (phone.name.empty()) printf("%s %s is not among the devices paired to Windows; matching by address only\n", tag, pretty(phone.address).c_str());
    const auto transport = find_transport(phone, tag);
    if (!transport) return 1;

    if (undo) {
        // Registration belongs to the calling app, so IsRegistered here means this tool registered it.
        // Windows keeps the Bluetooth link itself; this only removes the registration.
        try {
            if (!transport.IsRegistered()) { printf("%s IsRegistered: no; nothing of this tool's to undo\n", tag); return 0; }
            transport.UnregisterApp();
            printf("%s UnregisterApp: done; IsRegistered now: %s\n", tag, yes_no(transport.IsRegistered()));
            return 0;
        } catch (const hresult_error& e) { printf("%s UnregisterApp failed: %s\n", tag, failure(e).c_str()); return 1; }
    }
    bool registered_here = false; // kept after exit on purpose: disconnect undoes it
    return connect_steps(transport, tag, registered_here) ? 0 : 1;
}

// Moves this phone's talking call to the PC (like upstream phone_transport.cpp TransferActiveCall).
// Never dials, answers or touches calls of another phone.
int transfer(const std::wstring& text) {
    const char* tag = "[TRANSFER]";
    BtDevice phone;
    phone.address = normalize_address(text);
    if (phone.address.empty()) { printf("%s '%s' is not a Bluetooth address\n", tag, u8(text).c_str()); return 2; }
    for (const auto& d : paired_devices()) if (d.address == phone.address) phone = d;
    const auto transport = find_transport(phone, tag);
    if (!transport) return 1;
    const auto store = wait(PhoneCallManager::RequestStoreAsync());
    if (!store) { printf("%s Windows exposed no call store\n", tag); return 1; }
    std::vector<guid> ids;
    std::atomic<bool> done{false};
    std::mutex lock;
    auto watcher = store.RequestLineWatcher();
    watcher.LineAdded([&](auto const&, auto const& args) { std::lock_guard<std::mutex> g(lock); ids.push_back(args.LineId()); });
    watcher.EnumerationCompleted([&](auto const&, auto const&) { done = true; });
    watcher.Start();
    for (int i = 0; i < 200 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    watcher.Stop();
    std::vector<guid> lines;
    { std::lock_guard<std::mutex> g(lock); lines = ids; }
    for (const auto& id : lines) {
        const auto line = wait(PhoneLine::FromIdAsync(id));
        if (!line || line.TransportDeviceId() != transport.DeviceId()) continue;
        const auto result = wait(line.GetAllActivePhoneCallsAsync());
        if (result.OperationStatus() != PhoneLineOperationStatus::Succeeded) { printf("%s Cannot list calls (status %d)\n", tag, int(result.OperationStatus())); return 1; }
        for (const auto& call : result.AllActivePhoneCalls()) {
            printf("%s Call: status %d, audio on %s\n", tag, int(call.Status()), call.AudioDevice() == PhoneCallAudioDevice::LocalDevice ? "PC" : "phone");
            // Dialing too: Windows can miss the Q7's "answered" update and keep an outgoing call at Dialing.
            if (call.Status() != PhoneCallStatus::Talking && call.Status() != PhoneCallStatus::Dialing) continue;
            if (call.AudioDevice() == PhoneCallAudioDevice::LocalDevice) { printf("%s Already on the PC\n", tag); return 0; }
            const auto changed = wait(call.ChangeAudioDeviceAsync(PhoneCallAudioDevice::LocalDevice));
            printf("%s ChangeAudioDeviceAsync: %s (%d)\n", tag, changed == PhoneCallOperationStatus::Succeeded ? "moved to the PC" : "refused", int(changed));
            return changed == PhoneCallOperationStatus::Succeeded ? 0 : 1;
        }
        printf("%s No talking call on this phone's line\n", tag);
        return 1;
    }
    printf("%s Windows exposed no phone line for this phone (%zu line(s) in total)\n", tag, lines.size());
    return 1;
}

// ---- route ----

struct Failure { HRESULT hr; const char* step; };
void check(HRESULT hr, const char* step) { if (FAILED(hr)) throw Failure{hr, step}; }
const char* hr_name(HRESULT hr) {
    static const struct { HRESULT hr; const char* name; } names[] = {
        {AUDCLNT_E_DEVICE_INVALIDATED, "AUDCLNT_E_DEVICE_INVALIDATED"}, {AUDCLNT_E_DEVICE_IN_USE, "AUDCLNT_E_DEVICE_IN_USE"},
        {AUDCLNT_E_RESOURCES_INVALIDATED, "AUDCLNT_E_RESOURCES_INVALIDATED"}, {AUDCLNT_E_SERVICE_NOT_RUNNING, "AUDCLNT_E_SERVICE_NOT_RUNNING"},
        {AUDCLNT_E_ENDPOINT_CREATE_FAILED, "AUDCLNT_E_ENDPOINT_CREATE_FAILED"}, {AUDCLNT_E_UNSUPPORTED_FORMAT, "AUDCLNT_E_UNSUPPORTED_FORMAT"},
        {AUDCLNT_E_CPUUSAGE_EXCEEDED, "AUDCLNT_E_CPUUSAGE_EXCEEDED"}, {E_ACCESSDENIED, "E_ACCESSDENIED"},
        {HRESULT_FROM_WIN32(ERROR_NOT_FOUND), "ERROR_NOT_FOUND"}, {E_OUTOFMEMORY, "E_OUTOFMEMORY"}, {E_INVALIDARG, "E_INVALIDARG"}};
    for (const auto& n : names) if (n.hr == hr) return n.name;
    return "HRESULT";
}

struct Stream {
    const char* label = "";
    std::wstring id;
    bool capture = false;
    int tap = -1;              // call_tap channel for captures: 0 = other party, 1 = user
    AudioRing* ring = nullptr; // captures push, renders pull
    std::atomic<uint64_t> frames{0};
    std::atomic<int> peak{0};
    std::atomic<bool> running{false};
    std::atomic<HRESULT> error{S_OK};
    std::thread worker;
};

void note_audio(Stream& s, const int16_t* pcm, UINT32 frames) {
    int peak = 0;
    for (UINT32 i = 0; i < frames; ++i) peak = std::max(peak, std::abs(int(pcm[i])));
    for (int seen = s.peak; peak > seen && !s.peak.compare_exchange_weak(seen, peak);) {}
    s.frames += frames;
}

// Opens one endpoint as 8 kHz mono PCM16 and moves audio until stop. Throws Failure.
void stream_once(Stream& s, HANDLE stop, Failure& last) {
    com_ptr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put())), "open audio devices");
    com_ptr<IMMDevice> device;
    check(enumerator->GetDevice(s.id.c_str(), device.put()), "find endpoint");
    com_ptr<IAudioClient> client;
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()), "activate");
    // Call audio, so a Bluetooth endpoint uses its hands-free mode. Optional: ignore a refusal.
    if (const auto client2 = client.try_as<IAudioClient2>()) {
        AudioClientProperties properties{};
        properties.cbSize = sizeof(properties);
        properties.eCategory = AudioCategory_Communications;
        client2->SetClientProperties(&properties);
    }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 1; format.nSamplesPerSec = kRate;
    format.wBitsPerSample = 16; format.nBlockAlign = 2; format.nAvgBytesPerSec = kRate * 2;
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        300000, 0, &format, nullptr), "initialize 8 kHz mono");
    winrt::handle event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (!event) throw Failure{HRESULT_FROM_WIN32(GetLastError()), "create event"};
    check(client->SetEventHandle(event.get()), "set event");
    UINT32 capacity = 0;
    check(client->GetBufferSize(&capacity), "read buffer size");
    com_ptr<IAudioCaptureClient> capture;
    com_ptr<IAudioRenderClient> render;
    BYTE* data = nullptr;
    if (s.capture) {
        check(client->GetService(IID_PPV_ARGS(capture.put())), "get capture service");
    } else {
        check(client->GetService(IID_PPV_ARGS(render.put())), "get render service");
        check(render->GetBuffer(capacity, &data), "prime render buffer");
        check(render->ReleaseBuffer(capacity, AUDCLNT_BUFFERFLAGS_SILENT), "prime render buffer");
    }
    check(client->Start(), "start");
    unsigned mix_rate = 0;
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mix))) { mix_rate = mix->nSamplesPerSec; CoTaskMemFree(mix); }
    printf("[ROUTE] %s started (endpoint runs at %u Hz, converted to 8 kHz)\n", s.label, mix_rate);
    last = {S_OK, ""};
    s.error = S_OK;
    s.running = true;
    std::vector<int16_t> silence;
    const HANDLE waits[] = {stop, event.get()};
    for (;;) {
        // A timeout is not an error: an idle Bluetooth link may pause events, and the calls
        // below still report a removed or reconfigured endpoint.
        const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, 500);
        if (woke == WAIT_OBJECT_0) { client->Stop(); return; }
        if (woke == WAIT_FAILED) throw Failure{HRESULT_FROM_WIN32(GetLastError()), "wait for audio"};
        if (s.capture) {
            UINT32 frames = 0;
            check(capture->GetNextPacketSize(&frames), "read capture packet");
            while (frames) {
                DWORD flags = 0;
                check(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "get capture packet");
                const int16_t* pcm = reinterpret_cast<const int16_t*>(data);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) { silence.assign(frames, 0); pcm = silence.data(); }
                s.ring->push(pcm, frames);
                tap_push(s.tap, pcm, frames);
                note_audio(s, pcm, frames);
                check(capture->ReleaseBuffer(frames), "release capture packet");
                check(capture->GetNextPacketSize(&frames), "read capture packet");
            }
        } else {
            UINT32 padding = 0;
            check(client->GetCurrentPadding(&padding), "read render padding");
            const UINT32 available = capacity > padding ? capacity - padding : 0;
            if (!available) continue;
            check(render->GetBuffer(available, &data), "get render buffer");
            int16_t* pcm = reinterpret_cast<int16_t*>(data);
            s.ring->pull(pcm, available);
            note_audio(s, pcm, available);
            check(render->ReleaseBuffer(available, 0), "release render buffer");
        }
    }
}

// Keeps one endpoint streaming: on any failure it says which step and why, then retries
// every 2 s while the other three keep running.
void stream_worker(Stream& s, HANDLE stop) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Failure last{S_OK, ""};
    do {
        try { stream_once(s, stop, last); break; }
        catch (const Failure& f) {
            s.running = false;
            s.error = f.hr;
            if (f.hr != last.hr || std::strcmp(f.step, last.step)) {
                printf("[ROUTE] %s: %s failed: %s (0x%08lX)%s; retrying every 2 s\n", s.label, f.step, hr_name(f.hr), static_cast<unsigned long>(f.hr),
                       f.hr == E_ACCESSDENIED && s.capture ? " - check Settings > Privacy & security > Microphone > Let desktop apps access your microphone" : "");
                last = f;
            }
        }
    } while (WaitForSingleObject(stop, 2000) == WAIT_TIMEOUT);
    s.running = false;
    if (SUCCEEDED(com)) CoUninitialize();
}

std::atomic<HANDLE> g_stop{nullptr};
UINT g_codepage = 0;
// Ctrl+C while route runs stops it cleanly. Anywhere else the process ends as usual, after giving
// the console back its code page (the shell's window outlives this tool).
BOOL WINAPI on_console(DWORD) {
    if (const HANDLE stop = g_stop) { SetEvent(stop); return TRUE; }
    if (g_codepage) SetConsoleOutputCP(g_codepage);
    return FALSE;
}

std::string stream_status(const Stream& s, IMMDeviceEnumerator* enumerator) {
    std::string text = std::string(s.label) + '=' +
        (s.running ? "running" : s.error == S_OK ? "starting" : std::string("retry:") + hr_name(s.error));
    com_ptr<IMMDevice> device;
    DWORD state = 0;
    const bool found = SUCCEEDED(enumerator->GetDevice(s.id.c_str(), device.put())) && SUCCEEDED(device->GetState(&state));
    return text + '/' + (found ? state_name(state) : "missing");
}

void usage() {
    printf("Usage:\n"
           "  q7_winstack list                        paired Bluetooth devices, phone transports, all audio endpoints\n"
           "  q7_winstack connect <phone address>     ask Windows for this phone's call transport, step by step\n"
           "  q7_winstack transfer <phone address>    move the talking call to the PC\n"
           "  q7_winstack disconnect <phone address>  undo this tool's call transport registration\n"
           "  q7_winstack route [--phone <address|name>] [--headset <address|name>] [--seconds N] [--connect] [--stereo]\n"
           "                                          --connect: hold the phone's call transport connected while routing\n"
           "                                          --stereo: headset as stereo headphones, your voice from the PC's default mic\n"
           "                                                    (one call-audio link in total: the phone's)\n"
           "  q7_winstack selftest\n");
}

int route(int argc, wchar_t** argv) {
    std::wstring phone_wanted, headset_wanted;
    unsigned long seconds = 0;
    bool connect_first = false, stereo = false;
    for (int i = 0; i < argc; ++i) {
        const std::wstring flag = argv[i];
        if (flag == L"--connect") { connect_first = true; continue; }
        if (flag == L"--stereo") { stereo = true; continue; }
        if (i + 1 >= argc) { usage(); return 2; }
        const wchar_t* value = argv[++i];
        if (flag == L"--phone") phone_wanted = value;
        else if (flag == L"--headset") headset_wanted = value;
        else if (flag == L"--seconds") {
            wchar_t* end = nullptr;
            seconds = iswdigit(value[0]) ? wcstoul(value, &end, 10) : 0;
            if (!seconds || *end) { usage(); return 2; }
        } else { usage(); return 2; }
    }
    const auto devices = paired_devices();
    auto endpoints = audio_endpoints();
    int phones = 0, headsets = 0;
    const BtDevice* phone = pick_device(devices, endpoints, phone_wanted, kPhone, phones);
    const BtDevice* headset = pick_device(devices, endpoints, headset_wanted, kAudioVideo, headsets);
    if (!phone || !headset || phone == headset) {
        if (!phone) {
            if (phone_wanted.empty()) explain("[ROUTE] ", "phone", "--phone", phones);
            else printf("[ROUTE] --phone matches %d paired devices; it must match exactly one.\n", phones);
        }
        if (!headset) {
            if (headset_wanted.empty()) explain("[ROUTE] ", "headset", "--headset", headsets);
            else printf("[ROUTE] --headset matches %d paired devices; it must match exactly one.\n", headsets);
        }
        if (phone && phone == headset) printf("[ROUTE] --phone and --headset chose the same device.\n");
        printf("Paired Bluetooth devices:%s\n", devices.empty() ? " none (pair both in Settings > Bluetooth & devices)" : "");
        for (const auto& d : devices)
            printf("  %s  %s  class=%s audio endpoints=%s\n", pretty(d.address).c_str(), u8(d.name).c_str(), major_name(d.major), yes_no(has_endpoints(d, endpoints)));
        return 2;
    }
    // Flags skip the class check; swapped roles would put each voice on the other's call-audio
    // pipe channel. Unknown classes are accepted.
    if (phone->major == kAudioVideo && headset->major == kPhone) {
        printf("[ROUTE] --phone and --headset look swapped: %s is a headset and %s a phone.\n", u8(phone->name).c_str(), u8(headset->name).c_str());
        return 2;
    }

    PhoneLineTransportDevice transport{nullptr};
    bool registered_here = false;
    // Undo only what this run registered, like windows/runner/phone_transport.cpp.
    const auto release = [&] {
        if (!transport || !registered_here) return;
        try { transport.UnregisterApp(); printf("[ROUTE] UnregisterApp: done\n"); }
        catch (const hresult_error& e) { printf("[ROUTE] UnregisterApp failed: %s\n", failure(e).c_str()); }
    };
    if (connect_first) {
        // Held until the end: Windows may keep the phone's call link up only while an app holds it.
        transport = find_transport(*phone, "[ROUTE]");
        if (transport) connect_steps(transport, "[ROUTE]", registered_here);
        // ponytail: endpoints are read once, right after ConnectAsync; if Windows creates the phone's
        // endpoints later than that, run route again (they exist by then).
        endpoints = audio_endpoints();
    }
    const auto enumerator = create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator)); // default mic; endpoint states for the status line
    std::wstring mic;
    if (stereo) {
        com_ptr<IMMDevice> device;
        LPWSTR id = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device.put())) && SUCCEEDED(device->GetId(&id))) { mic = id; CoTaskMemFree(id); }
    }
    const auto picks = pick_streams(endpoints, *phone, *headset, stereo ? &mic : nullptr);
    if (std::find(picks.begin(), picks.end(), nullptr) != picks.end()) {
        for (size_t i = 0; i < picks.size(); ++i)
            if (!picks[i] && stereo && i == 2) printf("[ROUTE] The default microphone is missing or belongs to the phone or headset. "
                                                      "Set the PC's built-in microphone as default (Settings > Sound > Input), then run again.\n");
            else if (!picks[i]) printf("[ROUTE] No single clear %s endpoint (none, or several equally likely). "
                                       "Connect the device so its current endpoints are active, then run again.\n", kLabels[i]);
        for (const BtDevice* d : {phone, headset}) {
            printf("Endpoints of %s (%s):\n", u8(d->name).c_str(), pretty(d->address).c_str());
            for (const auto& e : endpoints) if (has_container(d->container) && e.container == d->container) print_endpoint(e, "");
        }
        release();
        return 2;
    }
    const std::string radio = sco_routing();
    printf("[ROUTE] Phone:   %s (%s)\n[ROUTE] Headset: %s (%s)\n[ROUTE] Radio:   %s\n", u8(phone->name).c_str(), pretty(phone->address).c_str(),
           u8(headset->name).c_str(), pretty(headset->address).c_str(), radio.c_str());
    for (size_t i = 0; i < picks.size(); ++i)
        printf("[ROUTE] %-11s %s [%s]\n", kLabels[i], u8(picks[i]->name).c_str(), state_name(picks[i]->state));

    // Left open on purpose: the console handler may still use it, and the process ends soon after.
    const HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) { printf("[ROUTE] Cannot create the stop event\n"); release(); return 1; }
    g_stop = stop;
    static AudioRing to_headset, to_phone;
    to_headset.reset(kRate);
    to_phone.reset(kRate);
    AudioRing* const rings[4] = {&to_headset, &to_headset, &to_phone, &to_phone};
    const int taps[4] = {0, -1, 1, -1};
    Stream streams[4];
    tap_open();
    for (size_t i = 0; i < 4; ++i) {
        streams[i].label = kLabels[i];
        streams[i].id = picks[i]->id;
        streams[i].capture = picks[i]->capture;
        streams[i].tap = taps[i];
        streams[i].ring = rings[i];
        streams[i].worker = std::thread(stream_worker, std::ref(streams[i]), stop);
    }
    printf("[ROUTE] Routing call audio. Ctrl+C stops.\n");
    uint64_t last[4] = {};
    int streak = 0, longest = 0;
    bool announced = false;
    for (unsigned long second = 1; WaitForSingleObject(stop, 1000) == WAIT_TIMEOUT; ++second) {
        std::array<Tick, 4> ticks;
        for (size_t i = 0; i < 4; ++i) {
            const uint64_t total = streams[i].frames;
            ticks[i] = {streams[i].running.load(), total - last[i], streams[i].peak.exchange(0)};
            last[i] = total;
        }
        streak = next_streak(streak, ticks);
        longest = std::max(longest, streak);
        printf("[ROUTE] %lus phone->headset in/out %llu/%llu fr peak %d/%d | headset->phone in/out %llu/%llu fr peak %d/%d | %s %s %s %s | together %d/%d s\n",
               second, (unsigned long long)ticks[0].frames, (unsigned long long)ticks[1].frames, ticks[0].peak, ticks[1].peak,
               (unsigned long long)ticks[2].frames, (unsigned long long)ticks[3].frames, ticks[2].peak, ticks[3].peak,
               stream_status(streams[0], enumerator.get()).c_str(), stream_status(streams[1], enumerator.get()).c_str(),
               stream_status(streams[2], enumerator.get()).c_str(), stream_status(streams[3], enumerator.get()).c_str(),
               std::min(streak, kVerdictSeconds), kVerdictSeconds);
        if (streak >= kVerdictSeconds && !announced) {
            announced = true;
            printf(stereo ? "[VERDICT] Call audio flows both ways (headset stereo + PC mic)\n" : "[VERDICT] Two call-audio links active at once\n");
        }
        if (seconds && second >= seconds) break;
    }
    SetEvent(stop);
    for (auto& s : streams) s.worker.join();
    tap_close();
    release();
    if (announced) printf("[ROUTE] Stopped. Longest stretch with both links carrying audio: %d s. Radio: %s\n", longest, radio.c_str());
    else printf("[RESULT] Both links never carried audio together for %d s (longest: %d s). Radio: %s\n", kVerdictSeconds, longest, radio.c_str());
    return announced ? 0 : 3;
}

// ---- selftest ----

int selftest() {
    int failed = 0;
#define EXPECT(condition) do { if (!(condition)) { printf("selftest FAILED line %d: %s\n", __LINE__, #condition); ++failed; } } while (0)
    EXPECT(normalize_address(L"f8:ed:ae:14:7b:5d") == L"F8EDAE147B5D");
    EXPECT(normalize_address(L"F8-ED-AE-14-7B-5D") == L"F8EDAE147B5D");
    EXPECT(normalize_address(L"F8EDAE147B5D") == L"F8EDAE147B5D");
    EXPECT(normalize_address(L"F8EDAE147B5").empty());
    EXPECT(normalize_address(L"soundcore").empty());
    EXPECT(pretty(L"880E852016C9") == "88:0E:85:20:16:C9");

    const GUID phone_box{1}, headset_box{2}, old_box{3}, machine{0, 0, 0, {0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    const std::vector<BtDevice> devices = {
        {L"F8EDAE147B5D", L"Q7", kPhone, 1, phone_box},
        {L"880E852016C9", L"soundcore P40i", kAudioVideo, 1, headset_box},
        {L"001122334455", L"Old phone", kPhone, 0, old_box}};
    std::vector<Endpoint> endpoints = {
        {L"p-in", L"Q7 in", true, DEVICE_STATE_ACTIVE, UnknownFormFactor, phone_box},
        {L"p-out", L"Q7 out", false, DEVICE_STATE_ACTIVE, UnknownFormFactor, phone_box},
        {L"h-stale", L"stale hands-free", false, DEVICE_STATE_NOTPRESENT, Headset, headset_box},
        {L"h-stereo", L"stereo only", false, DEVICE_STATE_ACTIVE, Headphones, headset_box},
        {L"h-out", L"hands-free out", false, DEVICE_STATE_ACTIVE, Headset, headset_box},
        {L"h-in", L"hands-free mic", true, DEVICE_STATE_ACTIVE, Headset, headset_box},
        {L"speakers", L"built-in", false, DEVICE_STATE_ACTIVE, Speakers, machine}};
    int matches = 0;
    // Auto: the old phone has no endpoints, so exactly one phone and one headset qualify.
    const BtDevice* phone = pick_device(devices, endpoints, L"", kPhone, matches);
    EXPECT(phone && phone->name == L"Q7");
    const BtDevice* headset = pick_device(devices, endpoints, L"", kAudioVideo, matches);
    EXPECT(headset && headset->name == L"soundcore P40i");
    if (phone && headset) {
        const auto picks = pick_streams(endpoints, *phone, *headset);
        const wchar_t* expected[4] = {L"p-in", L"h-out", L"h-in", L"p-out"}; // live hands-free beats stale and stereo-only
        for (size_t i = 0; i < 4; ++i) EXPECT(picks[i] && picks[i]->id == expected[i]);
    }
    // Flags: an address in any spelling or a case-insensitive name part; several matches are refused.
    EXPECT(pick_device(devices, endpoints, L"f8:ed:ae:14:7b:5d", kPhone, matches) == phone);
    EXPECT(pick_device(devices, endpoints, L"SOUNDCORE", kAudioVideo, matches) == headset);
    EXPECT(!pick_device(devices, endpoints, L"o", kPhone, matches) && matches == 2);
    // A second phone with endpoints, or two equally good endpoints, is never resolved by order.
    endpoints.push_back({L"old-in", L"old", true, DEVICE_STATE_UNPLUGGED, UnknownFormFactor, old_box});
    EXPECT(!pick_device(devices, endpoints, L"", kPhone, matches) && matches == 2);
    endpoints.push_back({L"h-out-2", L"twin", false, DEVICE_STATE_ACTIVE, Headset, headset_box});
    EXPECT(!pick_endpoint(endpoints, headset_box, false));
    EXPECT(!pick_endpoint(endpoints, GUID{}, true)); // a device without a container owns nothing
    // Hands-free still unplugged while the stereo endpoint is active: the hands-free one wins.
    // --stereo: the headset's stereo output and the PC mic; a mic of the phone or headset is refused.
    if (phone && headset) {
        const std::wstring built_in = L"built-in-mic", headset_mic = L"h-in";
        endpoints.push_back({built_in, L"PC mic", true, DEVICE_STATE_ACTIVE, Microphone, machine});
        const auto picks = pick_streams(endpoints, *phone, *headset, &built_in);
        EXPECT(picks[1] && picks[1]->id == L"h-stereo" && picks[2] && picks[2]->id == built_in);
        EXPECT(!pick_streams(endpoints, *phone, *headset, &headset_mic)[2]);
    }
    const GUID late_box{4};
    const std::vector<Endpoint> late = {{L"late-stereo", L"stereo", false, DEVICE_STATE_ACTIVE, Headphones, late_box},
                                        {L"late-hf", L"hands-free", false, DEVICE_STATE_UNPLUGGED, Headset, late_box}};
    const Endpoint* late_pick = pick_endpoint(late, late_box, false);
    EXPECT(late_pick && late_pick->id == L"late-hf");

    // Verdict: ten flowing seconds in a row; one stopped, idle or silent stream resets it.
    const std::array<Tick, 4> flowing = {{{true, 8000, 900}, {true, 8000, 850}, {true, 8000, 400}, {true, 8000, 380}}};
    int streak = 0;
    for (int i = 0; i < kVerdictSeconds; ++i) streak = next_streak(streak, flowing);
    EXPECT(streak == kVerdictSeconds);
    for (size_t i = 0; i < 4; ++i) {
        auto stopped = flowing, idle = flowing, silent = flowing;
        stopped[i].running = false; idle[i].frames = 0; silent[i].peak = 0;
        EXPECT(next_streak(streak, stopped) == 0 && next_streak(streak, idle) == 0 && next_streak(streak, silent) == 0);
    }
#undef EXPECT
    printf(failed ? "q7_winstack selftest: %d check(s) FAILED\n" : "q7_winstack selftest passed\n", failed);
    return failed ? 1 : 0;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_codepage = GetConsoleOutputCP();
    SetConsoleCtrlHandler(on_console, TRUE);
    SetConsoleOutputCP(CP_UTF8); // device and endpoint names are localized, often non-Latin
    int result = 2;
    try {
        init_apartment(apartment_type::multi_threaded);
        const std::wstring command = argc > 1 ? argv[1] : L"";
        if (command == L"list" && argc == 2) result = list();
        else if (command == L"transfer" && argc == 3) result = transfer(argv[2]);
        else if ((command == L"connect" || command == L"disconnect") && argc == 3) result = connect(argv[2], command == L"disconnect");
        else if (command == L"route") result = route(argc - 2, argv + 2);
        else if (command == L"selftest" && argc == 2) result = selftest();
        else usage();
    } catch (const hresult_error& e) {
        printf("Windows call failed: %s\n", failure(e).c_str());
        result = 1;
    }
    if (g_codepage) SetConsoleOutputCP(g_codepage);
    return result;
}
