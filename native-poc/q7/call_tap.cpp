// Live call-audio tap for a local listener app.
// Serves \\.\pipe\q7-call-audio: interleaved stereo PCM16 at 8 kHz,
// left = other party (from the phone), right = the user (headset/PC mic).
// Local-only (PIPE_REJECT_REMOTE_CLIENTS), current-user-only DACL, and
// FILE_FLAG_FIRST_PIPE_INSTANCE so another process cannot pre-create the name.
// Nothing is written to disk.
#include "call_tap.h"
#include <windows.h>
#include <sddl.h>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <cstdio>

namespace {
std::mutex lock;
std::deque<int16_t> queues[2];
std::atomic<bool> connected{false};
HANDLE stop_event = nullptr;
std::thread worker;
constexpr size_t kMaxBacklog = 8000; // 1 s
constexpr unsigned kFrames = 160;    // 20 ms

bool user_only_security(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    bool ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0;
    CloseHandle(token);
    if (!ok) return false;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) return false;
    std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return false;
    sa = {sizeof(sa), sd, FALSE};
    return true;
}

bool write_all(HANDLE pipe, HANDLE io_event, const void* data, DWORD size) {
    OVERLAPPED ov{}; ov.hEvent = io_event;
    DWORD written = 0;
    if (!WriteFile(pipe, data, size, &written, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        HANDLE waits[] = {stop_event, io_event};
        if (WaitForMultipleObjects(2, waits, FALSE, 1000) != WAIT_OBJECT_0 + 1) { CancelIo(pipe); return false; }
        if (!GetOverlappedResult(pipe, &ov, &written, FALSE)) return false;
    }
    return written == size;
}

void serve() {
    SECURITY_ATTRIBUTES sa{}; PSECURITY_DESCRIPTOR sd = nullptr;
    if (!user_only_security(sa, sd)) { printf("[TAP] Cannot build pipe security; tap disabled\n"); return; }
    HANDLE pipe = CreateNamedPipeW(L"\\\\.\\pipe\\q7-call-audio",
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 0, 0, &sa);
    LocalFree(sd);
    if (pipe == INVALID_HANDLE_VALUE) { printf("[TAP] Pipe unavailable (error %lu); tap disabled\n", GetLastError()); return; }
    HANDLE io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    printf("[TAP] Call-audio tap ready: \\\\.\\pipe\\q7-call-audio (stereo 8 kHz)\n");
    std::vector<int16_t> frame(kFrames * 2);
    for (;;) {
        OVERLAPPED ov{}; ov.hEvent = io_event; ResetEvent(io_event);
        BOOL linked = ConnectNamedPipe(pipe, &ov);
        DWORD error = GetLastError();
        if (!linked && error == ERROR_IO_PENDING) {
            HANDLE waits[] = {stop_event, io_event};
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) { CancelIo(pipe); break; }
        } else if (!linked && error != ERROR_PIPE_CONNECTED) break;
        { std::lock_guard<std::mutex> g(lock); queues[0].clear(); queues[1].clear(); }
        connected = true;
        printf("[TAP] Listener connected\n");
        while (WaitForSingleObject(stop_event, 20) == WAIT_TIMEOUT) {
            {
                std::lock_guard<std::mutex> g(lock);
                for (int c = 0; c < 2; ++c)
                    for (unsigned i = 0; i < kFrames; ++i) {
                        int16_t s = 0;
                        if (!queues[c].empty()) { s = queues[c].front(); queues[c].pop_front(); }
                        frame[i * 2 + c] = s;
                    }
            }
            if (!write_all(pipe, io_event, frame.data(), DWORD(frame.size() * sizeof(int16_t)))) break;
        }
        connected = false;
        DisconnectNamedPipe(pipe);
        printf("[TAP] Listener disconnected\n");
        if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) break;
    }
    CloseHandle(io_event);
    CloseHandle(pipe);
}
}

extern "C" void tap_open(void) {
    if (stop_event) return;
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event) return;
    worker = std::thread(serve);
}

extern "C" void tap_push(int channel, const int16_t* samples, unsigned count) {
    if (!connected || channel < 0 || channel > 1) return;
    std::lock_guard<std::mutex> g(lock);
    auto& q = queues[channel];
    q.insert(q.end(), samples, samples + count);
    while (q.size() > kMaxBacklog) q.pop_front();
}

extern "C" void tap_close(void) {
    if (!stop_event) return;
    SetEvent(stop_event);
    if (worker.joinable()) worker.join();
    CloseHandle(stop_event); stop_event = nullptr;
}
