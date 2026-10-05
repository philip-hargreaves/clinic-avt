#include "adapters/audio/media_foundation_reader.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off: the Media Foundation headers need windows.h first
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propkey.h>
#include <propsys.h>
// clang-format on
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <system_error>

#include "adapters/system/com_apartment.hpp"
#include "ports/audio_source.hpp"

namespace clinicavt::audio {

namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD kAudio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr DWORD kSource = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

// A damaged header can claim any length, so cap the reserve at 4 hours
constexpr double kMostSecondsReserved = 4 * 3600;

[[noreturn]] void Refuse(HRESULT hr, const char* otherwise) {
    switch (hr) {
        case MF_E_UNSUPPORTED_BYTESTREAM_TYPE:
        case MF_E_INVALID_FILE_FORMAT:
        case MF_E_UNSUPPORTED_FORMAT:
            throw RecordingError(
                "it is not a kind of recording Windows can read. Dictation (DSS) files and voice "
                "messages from chat apps need saving as MP3 or WAV first");
        case MF_E_INVALIDSTREAMNUMBER:
            throw RecordingError("the file holds no sound");
        case MF_E_INVALIDMEDIATYPE:
        case MF_E_TOPO_CODEC_NOT_FOUND:
        case MF_E_NO_MORE_TYPES:
            throw RecordingError("Windows has no decoder for the sound in it");
        case MF_E_INVALID_POSITION:  // a cut-short MP4 points past its end
        case MF_E_INVALID_STREAM_DATA:
            throw RecordingError("the file is damaged");
        default:
            break;
    }
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        switch (HRESULT_CODE(hr)) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
            case ERROR_INVALID_NAME:
                throw RecordingError("the file is no longer there");
            case ERROR_ACCESS_DENIED:
            case ERROR_SHARING_VIOLATION:
            case ERROR_LOCK_VIOLATION:
                throw RecordingError(
                    "the file could not be opened; another program may be using it");
            default:
                break;
        }
    }
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    throw RecordingError(std::string(otherwise) + " (" + code + ")");
}

void Check(HRESULT hr, const char* otherwise) {
    if (FAILED(hr)) Refuse(hr, otherwise);
}

// Windows N, KN and Server can lack Media Foundation. The probe avoids the SEH
// exception a failed delay load raises
void RequireMediaFoundation() {
    for (const wchar_t* dll : {L"mfplat.dll", L"mfreadwrite.dll"}) {
        const HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            throw RecordingError(
                "this edition of Windows cannot read recordings until the Media Feature Pack is "
                "added (Settings, Apps, Optional features)");
        }
        FreeLibrary(module);
    }
}

// COM and Media Foundation for one call. Both are reference counted
class MediaFoundation {
   public:
    MediaFoundation() {
        // A thread already in another apartment still serves Media Foundation
        if (FAILED(com_.hr) && com_.hr != RPC_E_CHANGED_MODE) {
            Refuse(com_.hr, "Windows could not start reading it");
        }
        RequireMediaFoundation();
        Check(MFStartup(MF_VERSION, MFSTARTUP_LITE), "Windows could not start reading it");
        started_ = true;
    }
    ~MediaFoundation() {
        if (started_) MFShutdown();
    }
    MediaFoundation(const MediaFoundation&) = delete;
    MediaFoundation& operator=(const MediaFoundation&) = delete;

   private:
    system::ComApartment com_;
    bool started_ = false;
};

bool IsPipelineFormat(IMFSourceReader& reader) {
    ComPtr<IMFMediaType> type;
    if (FAILED(reader.GetCurrentMediaType(kAudio, &type))) return false;
    GUID subtype{};
    UINT32 rate = 0;
    UINT32 channels = 0;
    return SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFAudioFormat_Float &&
           SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) &&
           rate == static_cast<UINT32>(kSampleRate) &&
           SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) && channels == 1;
}

// The reader inserts the decoder, resampler and downmix, which averages the channels
void RequestPipelineFormat(IMFSourceReader& reader) {
    ComPtr<IMFMediaType> want;
    Check(MFCreateMediaType(&want), "Windows could not start reading it");
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    want->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
    want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    want->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, sizeof(float));
    want->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kSampleRate * sizeof(float));
    want->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    Check(reader.SetCurrentMediaType(kAudio, nullptr, want.Get()),
          "Windows could not decode the sound in it");
    if (!IsPipelineFormat(reader)) {
        throw RecordingError("Windows could not convert the sound in it");
    }
}

ComPtr<IMFSourceReader> OpenAudio(const std::filesystem::path& path) {
    std::error_code ec;
    const auto full = std::filesystem::absolute(path, ec);
    ComPtr<IMFSourceReader> reader;
    Check(MFCreateSourceReaderFromURL((ec ? path : full).c_str(), nullptr, &reader),
          "it could not be opened");
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    Check(reader->SetStreamSelection(kAudio, TRUE), "the file holds no sound");
    RequestPipelineFormat(*reader.Get());
    return reader;
}

// Container duration estimate. A VBR MP3 without a header can be ~20% short
double Seconds(IMFSourceReader& reader) {
    PROPVARIANT value;
    PropVariantInit(&value);
    double seconds = 0;
    if (SUCCEEDED(reader.GetPresentationAttribute(kSource, MF_PD_DURATION, &value)) &&
        value.vt == VT_UI8) {
        seconds = static_cast<double>(value.uhVal.QuadPart) / 1e7;
    }
    PropVariantClear(&value);
    return seconds;
}

std::chrono::sys_seconds FromFileTime(const FILETIME& time) {
    constexpr std::int64_t kUnixEpoch = 116444736000000000;  // 1970 in 100 ns ticks from 1601
    const std::int64_t ticks =
        (static_cast<std::int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return std::chrono::sys_seconds{std::chrono::seconds{(ticks - kUnixEpoch) / 10'000'000}};
}

// When recording began, as the container states it (an MP4's creation time)
std::optional<std::chrono::sys_seconds> EncodedAt(IMFSourceReader& reader) {
    ComPtr<IPropertyStore> props;
    if (FAILED(reader.GetServiceForStream(kSource, MF_PROPERTY_HANDLER_SERVICE,
                                          IID_PPV_ARGS(&props)))) {
        return std::nullopt;
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::optional<std::chrono::sys_seconds> out;
    if (SUCCEEDED(props->GetValue(PKEY_Media_DateEncoded, &value)) && value.vt == VT_FILETIME) {
        out = FromFileTime(value.filetime);
    }
    PropVariantClear(&value);
    return out;
}

}  // namespace

RecordingInfo MediaFoundationReader::Inspect(const std::filesystem::path& path) {
    const MediaFoundation mf;
    const auto reader = OpenAudio(path);
    RecordingInfo info;
    info.seconds = Seconds(*reader.Get());
    if (const auto encoded = EncodedAt(*reader.Get())) {
        info.recorded_at = *encoded;
        return info;
    }
    // File write time is when recording ended
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path, ec);
    if (ec) throw RecordingError("the file could not be opened");
    const auto ended = std::chrono::floor<std::chrono::seconds>(
        std::chrono::clock_cast<std::chrono::system_clock>(written));
    info.recorded_at = ended - std::chrono::seconds{std::llround(info.seconds)};
    return info;
}

std::vector<float> MediaFoundationReader::Decode(const std::filesystem::path& path,
                                                 const ReadProgress& progress) {
    const MediaFoundation mf;
    const auto reader = OpenAudio(path);

    const double seconds = Seconds(*reader.Get());
    std::vector<float> audio;
    audio.reserve(static_cast<std::size_t>(std::min(seconds, kMostSecondsReserved) * kSampleRate));
    double reported = 0;
    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = reader->ReadSample(kAudio, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
            Refuse(FAILED(hr) ? hr : E_FAIL, "the file is damaged");
        }
        // A new type at the same rate is harmless. Any other plays at the wrong speed
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0 &&
            !IsPipelineFormat(*reader.Get())) {
            throw RecordingError("the sound in it changes format partway through");
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) break;
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buffer;
        Check(sample->ConvertToContiguousBuffer(&buffer), "the file is damaged");
        BYTE* bytes = nullptr;
        DWORD length = 0;
        Check(buffer->Lock(&bytes, nullptr, &length), "the file is damaged");
        const auto* first = reinterpret_cast<const float*>(bytes);
        const std::size_t count = length / sizeof(float);
        audio.insert(audio.end(), first, first + count);
        buffer->Unlock();
        // Samples are a few ms each, so progress is reported in 1% steps
        const double done = static_cast<double>(audio.size()) / kSampleRate / seconds;
        if (progress && seconds > 0 && done - reported >= 0.01) {
            reported = done;
            progress(std::min(done, 1.0));
        }
    }
    if (audio.empty()) throw RecordingError("the file holds no sound");
    return audio;
}

}  // namespace clinicavt::audio
