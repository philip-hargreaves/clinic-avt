#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "ports/audio_source.hpp"

namespace clinicavt::session {

// A replay request passed to the source factory. Without one the source is the microphone
struct ReplaySpec {
    std::string path;
    double speed = 1.0;
    std::uint64_t start_frame = 0;  // on resume, skips audio already captured
};

// Resolved by the caller. The id pins the endpoint (empty means the default) and the name goes into
// the session snapshot
struct MicSelection {
    std::string id;
    std::string name;
};

using SourceFactory = std::function<std::unique_ptr<audio::IAudioSource>(
    const std::optional<ReplaySpec>&, const std::string& mic_id)>;

}  // namespace clinicavt::session
