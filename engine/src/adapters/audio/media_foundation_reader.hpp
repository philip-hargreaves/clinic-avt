#pragma once

#include <filesystem>
#include <vector>

#include "adapters/interfaces/recording_reader.hpp"

namespace clinicavt::audio {

// Delay-loaded, so a missing Media Foundation only breaks this reader
class MediaFoundationReader : public IRecordingReader {
   public:
    RecordingInfo Inspect(const std::filesystem::path& path) override;
    std::vector<float> Decode(const std::filesystem::path& path,
                              const ReadProgress& progress) override;
};

}  // namespace clinicavt::audio
