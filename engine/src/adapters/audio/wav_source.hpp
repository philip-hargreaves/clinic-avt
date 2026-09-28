#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "ports/audio_source.hpp"

namespace clinicavt::audio {

// Outside WavSource, so its initialisers are complete where the constructor defaults it
struct WavSourceConfig {
    double speed = 0.0;             // 1 = real time, >1 faster, 0 = as fast as possible
    std::uint64_t start_frame = 0;  // resume: skip audio already captured
};

// Plays a mono 16 kHz wav through the audio port. Used as the CI mic and for replay
class WavSource : public IAudioSource {
   public:
    using Config = WavSourceConfig;

    // path is UTF-8, as the wire carries it
    explicit WavSource(std::string path, Config config = {});

    void Run(IAudioSink& sink) override;
    void RequestStop() override;

   private:
    SourceEnd RunToEnd(IAudioSink& sink);

    std::string path_;
    Config config_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace clinicavt::audio
