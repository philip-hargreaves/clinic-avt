#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/audio/audio_ring.hpp"
#include "ports/audio_source.hpp"

namespace clinicavt::audio {

// Moves processing off the source thread. OnAudio copies into a ring and a consumer thread feeds
// the inner sink. An inner stall is absorbed up to ring capacity, then frames are dropped and
// counted as lost. OnEnd drains the ring and runs the inner OnEnd before returning
class BufferedSink : public IAudioSink {
   public:
    BufferedSink(IAudioSink& inner, std::size_t capacity_frames)
        : inner_(inner),
          ring_(capacity_frames),
          scratch_(std::min(ring_.Capacity(), kChunkFrames)) {
        consumer_ = std::thread([this] { Consume(); });
    }

    ~BufferedSink() override {
        if (consumer_.joinable()) {
            OnEnd({SourceEndReason::kStopped, ""});
        }
    }

    BufferedSink(const BufferedSink&) = delete;
    BufferedSink& operator=(const BufferedSink&) = delete;

    // Runs on the source thread and never blocks. Frames that don't fit are counted as lost.
    // Source-reported loss is added before the push so it goes with these frames. Overrun loss is
    // added after, so it goes with the next batch
    void OnAudio(std::span<const float> frames, std::uint64_t lost_frames) override {
        if (lost_frames > 0) {
            pending_lost_.fetch_add(lost_frames, std::memory_order_relaxed);
        }
        const std::size_t pushed = ring_.TryPush(frames);
        if (pushed < frames.size()) {
            pending_lost_.fetch_add(frames.size() - pushed, std::memory_order_relaxed);
        }
        Wake();
    }

    // Runs on the source thread. Blocks until the ring is drained and the inner OnEnd has run
    void OnEnd(const SourceEnd& end) override {
        end_ = end;
        ended_.store(true, std::memory_order_release);
        Wake();
        if (consumer_.joinable()) {
            consumer_.join();
        }
    }

   private:
    void Wake() {
        wake_.store(1, std::memory_order_release);
        wake_.notify_one();
    }

    void Consume() {
        try {
            for (;;) {
                wake_.wait(0, std::memory_order_acquire);
                wake_.store(0, std::memory_order_relaxed);
                // Drain before checking ended_ so the end can't be delivered ahead of queued audio
                for (;;) {
                    const std::size_t n = ring_.TryPop(scratch_);
                    if (n == 0) {
                        break;
                    }
                    const std::uint64_t lost = pending_lost_.exchange(0, std::memory_order_relaxed);
                    inner_.OnAudio(std::span<const float>(scratch_.data(), n), lost);
                }
                if (ended_.load(std::memory_order_acquire)) {
                    inner_.OnEnd(end_);
                    return;
                }
            }
        } catch (const std::exception& e) {
            inner_.OnEnd(
                {SourceEndReason::kFailed, std::string("audio pipeline threw: ") + e.what()});
        } catch (...) {
            inner_.OnEnd({SourceEndReason::kFailed, "audio pipeline threw"});
        }
    }

    // At most 1 s per inner call, to bound the work queued behind a stall
    static constexpr std::size_t kChunkFrames = static_cast<std::size_t>(kSampleRate);

    IAudioSink& inner_;
    AudioRing ring_;
    std::vector<float> scratch_;  // consumer only
    std::atomic<std::uint64_t> pending_lost_{0};
    std::atomic<int> wake_{0};
    std::atomic<bool> ended_{false};
    SourceEnd end_;  // written before ended_, read after
    std::thread consumer_;
};

}  // namespace clinicavt::audio
