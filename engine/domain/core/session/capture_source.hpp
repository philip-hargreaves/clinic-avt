#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "ports/audio_source.hpp"

namespace clinicavt::session {

// A replay request, carried into the source factory. Absent means microphone
struct ReplaySpec {
    std::string path;
    double speed = 1.0;
    std::uint64_t start_frame = 0;  // resume: skip audio already captured
};

// Resolved by the caller: the id pins the endpoint (empty = default), the
// name goes into the session snapshot
struct MicSelection {
    std::string id;
    std::string name;
};

using SourceFactory = std::function<std::unique_ptr<audio::IAudioSource>(
    const std::optional<ReplaySpec>&, const std::string& mic_id)>;

}  // namespace clinicavt::session
