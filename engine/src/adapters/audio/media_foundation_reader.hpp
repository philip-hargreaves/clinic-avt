#pragma once

#include <filesystem>
#include <vector>

#include "ports/recording_reader.hpp"

namespace clinicavt::audio {

// Reads a recording through Windows Media Foundation, which picks the decoder
// from the file's content and converts to the pipeline's format itself.
// Media Foundation is delay-loaded, so its absence refuses only this reader
class MediaFoundationReader : public IRecordingReader {
   public:
    RecordingInfo Inspect(const std::filesystem::path& path) override;
    std::vector<float> Decode(const std::filesystem::path& path,
                              const ReadProgress& progress) override;
};

}  // namespace clinicavt::audio
