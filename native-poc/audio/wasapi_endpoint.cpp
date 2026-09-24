#include "wasapi_internal.h"
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <string>
void check_audio(HRESULT hr, const char* step) {
    if (FAILED(hr)) { char error[180]; sprintf_s(error, "%s HRESULT=0x%08lx", step, static_cast<unsigned long>(hr)); throw std::runtime_error(error); }
}
WasapiEndpoint::WasapiEndpoint(bool capture, unsigned rate, unsigned channels) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    check_audio(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "Enumerate audio");
    // Resolve defaults once per SCO link. Never silently switch endpoints mid-call.
    wchar_t selected[1024] = {};
    const DWORD length = GetEnvironmentVariableW(capture ? L"AX201_CAPTURE" : L"AX201_RENDER", selected, 1024);
    if (length && length < 1024) check_audio(enumerator->GetDevice(selected, &device), "Get selected endpoint");
    else check_audio(enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eCommunications, &device), "Get default endpoint");
    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
        PROPVARIANT name; PropVariantInit(&name);
        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
            printf("[AUDIO] %s endpoint: %ls\n", capture ? "Capture" : "Playback", name.pwszVal);
        PropVariantClear(&name);
    }
    check_audio(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client), "Activate audio client");
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mix))) {
        printf("[QUALITY] Endpoint mix=%lu Hz channels=%u bits=%u; Bluetooth PCM=%u Hz\n",
            mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample, rate);
        CoTaskMemFree(mix);
    }
    ComPtr<IAudioClient2> client2;
    bool raw_requested = false;
    if (SUCCEEDED(client.As(&client2))) {
        AudioClientProperties properties = {};
        properties.cbSize = sizeof(properties);
        properties.eCategory = capture ? AudioCategory_Communications : AudioCategory_Media;
        properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
        const HRESULT raw = client2->SetClientProperties(&properties);
        raw_requested = SUCCEEDED(raw);
        printf("[QUALITY] %s RAW processing %s (0x%08lx)\n", capture ? "Capture" : "Playback",
            SUCCEEDED(raw) ? "enabled" : "unavailable; default processing retained", (unsigned long)raw);
    }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = static_cast<WORD>(channels); format.nSamplesPerSec = rate;
    format.wBitsPerSample = 16; format.nBlockAlign = static_cast<WORD>(channels * 2); format.nAvgBytesPerSec = rate * channels * 2;
    const DWORD flags =
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    HRESULT initialized = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 300000, 0, &format, nullptr);
    if (FAILED(initialized) && raw_requested) {
        printf("[QUALITY] RAW initialization failed (0x%08lx); retrying default processing\n", (unsigned long)initialized);
        client2.Reset(); client.Reset();
        check_audio(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client), "Reactivate audio client");
        initialized = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 300000, 0, &format, nullptr);
    }
    check_audio(initialized, "Initialize shared PCM with resampling");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("Create audio event failed");
    const HRESULT hr = client->SetEventHandle(event);
    if (FAILED(hr)) { CloseHandle(event); event = nullptr; check_audio(hr, "Set audio event"); }
    const HRESULT size_hr = client->GetBufferSize(&capacity);
    if (FAILED(size_hr)) { CloseHandle(event); event = nullptr; check_audio(size_hr, "Get buffer size"); }
}
WasapiEndpoint::~WasapiEndpoint() { if (client) client->Stop(); if (event) CloseHandle(event); }
