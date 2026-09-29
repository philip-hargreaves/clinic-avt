#pragma once

#include <atomic>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "ports/audio_source.hpp"

namespace clinicavt::audio {

// Replays a crashed session's stored audio at full speed, then runs the live source on
// the same sink
class ResumeSource : public IAudioSource {
   public:
    ResumeSource(std::vector<float> stored, std::unique_ptr<IAudioSource> live)
        : stored_(std::move(stored)), live_(std::move(live)) {}

    void Run(IAudioSink& sink) override {
        constexpr std::size_t kPacketFrames = 480;
        std::size_t at = 0;
        while (at < stored_.size()) {
            if (stop_requested_.load(std::memory_order_relaxed)) {
                sink.OnEnd({SourceEndReason::kStopped, ""});
                return;
            }
            const std::size_t count = std::min(kPacketFrames, stored_.size() - at);
            sink.OnAudio(std::span<const float>(stored_.data() + at, count), 0);
            at += count;
        }
        if (stop_requested_.load(std::memory_order_relaxed)) {
            sink.OnEnd({SourceEndReason::kStopped, ""});
            return;
        }
        live_->Run(sink);
    }

    void RequestStop() override {
        stop_requested_.store(true, std::memory_order_relaxed);
        live_->RequestStop();
    }

   private:
    std::vector<float> stored_;
    std::unique_ptr<IAudioSource> live_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace clinicavt::audio
