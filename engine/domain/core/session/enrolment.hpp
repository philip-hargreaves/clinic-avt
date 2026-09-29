#pragma once

#include <memory>
#include <mutex>
#include <thread>

#include "core/session/capture_source.hpp"
#include "ports/audio_source.hpp"
#include "ports/diariser.hpp"
#include "ports/session_events.hpp"
#include "ports/streaming_vad.hpp"

namespace clinicavt::session {

// Records until Finish or the time cap, embeds the speech and replaces the voice anchor.
// Reports via events. Caller must not run it alongside recording
class Enrolment {
   public:
    Enrolment(const SourceFactory& factory, audio::IStreamingVad& vad, diar::IDiariser& diariser,
              ISessionEvents& events);
    ~Enrolment();
    Enrolment(const Enrolment&) = delete;
    Enrolment& operator=(const Enrolment&) = delete;

    // False if already running
    bool Start(double seconds, const MicSelection& mic, double min_speech_s);
    void Cancel();
    // Stops early and builds the print from what was captured
    void Finish();
    bool Running() const;
    // Waits for a running enrolment to end
    void Join();

   private:
    void Run(double seconds, const MicSelection& mic, double min_speech_s);

    const SourceFactory& factory_;
    audio::IStreamingVad& vad_;
    diar::IDiariser& diariser_;
    ISessionEvents& events_;
    mutable std::mutex mutex_;
    std::unique_ptr<audio::IAudioSource> source_;  // under mutex_
    std::thread thread_;
    bool running_ = false;  // under mutex_
    bool cancel_ = false;   // under mutex_
    bool finish_ = false;   // under mutex_
};

}  // namespace clinicavt::session
