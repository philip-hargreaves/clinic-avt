#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace clinicavt::audio {

struct RecordingInfo {
    double seconds = 0;
    std::chrono::sys_seconds recorded_at{};  // the file's own date, else when it was last written
};

// The reason is plain words for the clinician. A file's name or content never goes in it
class RecordingError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

// How far a read has got, 0 to 1
using ReadProgress = std::function<void(double fraction)>;

// Reads an audio file made elsewhere into the engine's 16 kHz mono
class IRecordingReader {
   public:
    virtual ~IRecordingReader() = default;
    virtual RecordingInfo Inspect(const std::filesystem::path& path) = 0;
    virtual std::vector<float> Decode(const std::filesystem::path& path,
                                      const ReadProgress& progress) = 0;
};

}  // namespace clinicavt::audio
