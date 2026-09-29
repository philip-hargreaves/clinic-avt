#include "adapters/audio/wasapi_capture.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off: appmodel.h needs windows.h first
#include <windows.h>
#include <appmodel.h>
// clang-format on
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "adapters/audio/capture_errors.hpp"
#include "adapters/audio/capture_timeline.hpp"
#include "adapters/system/com_apartment.hpp"
#include "core/common/log.hpp"

namespace clinicavt::audio {

namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD kStreamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                               AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                               AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

SourceEnd Fail(const char* what, HRESULT hr) {
    return EndForCaptureError(what, static_cast<std::uint32_t>(hr));
}

std::wstring ConsentStoreValue(const std::wstring& subkey) {
    wchar_t value[16]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), L"Value", RRF_RT_REG_SZ, nullptr, value,
                     &size) != ERROR_SUCCESS) {
        return L"";
    }
    return value;
}

// Windows records the mic privacy toggle but does not enforce it for
// full-trust processes (measured), so the engine enforces it itself
bool ConsentDenied() {
    const std::wstring store =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore"
        L"\\microphone";
    if (ConsentStoreValue(store) == L"Deny") {
        return true;
    }

    wchar_t family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1]{};
    UINT32 length = PACKAGE_FAMILY_NAME_MAX_LENGTH + 1;
    if (GetCurrentPackageFamilyName(&length, family) == ERROR_SUCCESS) {
        return ConsentStoreValue(store + L"\\" + family) == L"Deny";
    }
    return ConsentStoreValue(store + L"\\NonPackaged") == L"Deny";
}

struct OwnedHandle {
    HANDLE handle = nullptr;
    ~OwnedHandle() {
        if (handle != nullptr) CloseHandle(handle);
    }
};

struct StopOnExit {
    IAudioClient* client;
    ~StopOnExit() {
        client->Stop();
    }
};

}  // namespace

WasapiCapture::WasapiCapture(std::wstring endpoint_id) : endpoint_id_(std::move(endpoint_id)) {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

WasapiCapture::~WasapiCapture() {
    if (stop_event_ != nullptr) {
        CloseHandle(stop_event_);
    }
}

void WasapiCapture::RequestStop() {
    if (stop_event_ != nullptr) {
        SetEvent(stop_event_);
    }
}

void WasapiCapture::Run(IAudioSink& sink) {
    sink.OnEnd(RunToEnd(sink));
}

SourceEnd WasapiCapture::RunToEnd(IAudioSink& sink) {
    if (stop_event_ == nullptr) {
        return {SourceEndReason::kFailed, "stop event could not be created"};
    }

    const system::ComApartment com;
    if (FAILED(com.hr)) {
        return Fail("CoInitializeEx", com.hr);
    }

    if (ConsentDenied()) {
        return {SourceEndReason::kFailed, "microphone access denied in Windows privacy settings"};
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        return Fail("CoCreateInstance", hr);
    }

    // Resolved once, then pinned for the whole stream
    ComPtr<IMMDevice> device;
    hr = endpoint_id_.empty()
             ? enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device)
             : enumerator->GetDevice(endpoint_id_.c_str(), &device);
    if (FAILED(hr)) {
        return Fail(endpoint_id_.empty() ? "GetDefaultAudioEndpoint" : "GetDevice", hr);
    }

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) {
        return Fail("Activate", hr);
    }

    // Without the communications category Windows never wakes a Bluetooth mic
    // link (measured: silence). Side effect: other apps' audio ducks while recording
    ComPtr<IAudioClient2> client2;
    if (SUCCEEDED(client.As(&client2))) {
        AudioClientProperties properties{};
        properties.cbSize = sizeof(properties);
        properties.eCategory = AudioCategory_Communications;
        client2->SetClientProperties(&properties);  // best effort
    }

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) {
        return Fail("GetMixFormat", hr);
    }
    const std::uint32_t native_rate = mix->nSamplesPerSec;
    CoTaskMemFree(mix);

    WAVEFORMATEX wanted{};
    wanted.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wanted.nChannels = 1;
    wanted.nSamplesPerSec = static_cast<DWORD>(kSampleRate);
    wanted.wBitsPerSample = 32;
    wanted.nBlockAlign = 4;
    wanted.nAvgBytesPerSec = wanted.nSamplesPerSec * wanted.nBlockAlign;

    const auto t_init = std::chrono::steady_clock::now();
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, kStreamFlags, 0, 0, &wanted, nullptr);
    if (FAILED(hr)) {
        return Fail("Initialize", hr);
    }
    // Logs native rate and open time so a dead or slow mic (Bluetooth can take
    // seconds to wake) shows in the log
    log::Printf("clinicavt-engine: capture open, native %lu Hz, %.2f s\n",
                static_cast<unsigned long>(native_rate),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t_init).count());

    OwnedHandle audio_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (audio_event.handle == nullptr) {
        return Fail("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
    }
    hr = client->SetEventHandle(audio_event.handle);
    if (FAILED(hr)) {
        return Fail("SetEventHandle", hr);
    }

    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr)) {
        return Fail("GetService", hr);
    }

    UINT32 buffer_frames = 0;
    hr = client->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) {
        return Fail("GetBufferSize", hr);
    }
    std::vector<float> packet(buffer_frames);

    hr = client->Start();
    if (FAILED(hr)) {
        return Fail("Start", hr);
    }
    const StopOnExit stopper{client.Get()};

    CaptureTimeline timeline(native_rate);
    std::uint64_t stream_packets = 0, stream_silent = 0;
    float stream_peak = 0;
    const HANDLE waits[2] = {static_cast<HANDLE>(stop_event_), audio_event.handle};
    for (;;) {
        const DWORD wake = WaitForMultipleObjects(2, waits, FALSE, 2000);
        if (wake == WAIT_OBJECT_0) {
            // peak 0.0000 with packets flowing is the LE Audio failure (unflagged zeros)
            log::Printf("clinicavt-engine: capture end: %llu packets, %llu silent, peak %.4f\n",
                        static_cast<unsigned long long>(stream_packets),
                        static_cast<unsigned long long>(stream_silent), stream_peak);
            return {SourceEndReason::kStopped, ""};
        }
        if (wake == WAIT_FAILED) {
            return Fail("WaitForMultipleObjects", HRESULT_FROM_WIN32(GetLastError()));
        }
        // On timeout, still drain: a dead device stops signalling and only a capture
        // call returns its error

        for (;;) {
            UINT32 next = 0;
            hr = capture->GetNextPacketSize(&next);
            if (FAILED(hr)) {
                return Fail("GetNextPacketSize", hr);
            }
            if (next == 0) {
                break;
            }

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 position = 0;
            UINT64 qpc = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, &position, &qpc);
            if (FAILED(hr)) {
                return Fail("GetBuffer", hr);
            }

            const std::uint64_t lost = timeline.OnPacket(position, frames);
            if (frames > packet.size()) {
                packet.resize(frames);
            }
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                std::memset(packet.data(), 0, frames * sizeof(float));
            } else {
                std::memcpy(packet.data(), data, frames * sizeof(float));
            }
            ++stream_packets;
            stream_silent += (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 ? 1 : 0;
            for (UINT32 i = 0; i < frames; ++i) {
                stream_peak = std::max(stream_peak, std::abs(packet[i]));
            }

            // Release within the buffer period; the sink gets the copy after
            hr = capture->ReleaseBuffer(frames);
            if (FAILED(hr)) {
                return Fail("ReleaseBuffer", hr);
            }
            sink.OnAudio(std::span<const float>(packet.data(), frames), lost);
        }
    }
}

}  // namespace clinicavt::audio
