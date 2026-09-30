#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

#include "ports/audio_source.hpp"
#include "ports/session_store.hpp"

namespace clinicavt::session {

// One mutex guards all of it for the controller and its parts. The condition variable wakes the
// start wait and the diarisation loop
struct SessionState {
    mutable std::mutex mutex;
    std::condition_variable cv;
    bool running = false;
    bool got_audio = false;
    bool ended = false;
    bool stop_requested = false;
    bool importing = false;
    std::uint64_t lost_frames = 0;
    int diar_ticks = 0;  // diagnostics
    store::SessionId session_id;
    store::SessionId resumed_from;
    store::SessionId last_finalised;
    bool reviewing = false;  // last_finalised came from Open
    // Appended under mutex (the diarisation thread snapshots it). Finalise
    // reads it after every other thread has joined
    std::vector<float> session_audio;
    audio::SourceEnd end{};

    bool Running() const {
        std::lock_guard<std::mutex> lock(mutex);
        return running && !ended;
    }

    store::SessionId Current() const {
        std::lock_guard<std::mutex> lock(mutex);
        return session_id;
    }

    store::SessionId LastFinalised() const {
        std::lock_guard<std::mutex> lock(mutex);
        return last_finalised;
    }
};

}  // namespace clinicavt::session
