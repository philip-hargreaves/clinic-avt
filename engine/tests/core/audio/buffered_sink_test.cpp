#include "core/audio/buffered_sink.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include "ports/audio_source.hpp"

namespace clinicavt::audio {
namespace {

// Records packets in order. Set hold to block OnAudio so the ring fills
struct RecordingSink : IAudioSink {
    std::vector<float> frames;
    std::vector<std::uint64_t> lost_per_packet;
    std::vector<SourceEnd> ends;
    std::atomic<bool> hold{false};
    std::atomic<bool> throw_next{false};
    mutable std::mutex mutex;

    void OnAudio(std::span<const float> packet, std::uint64_t lost) override {
        while (hold.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (throw_next.exchange(false)) {
            throw std::runtime_error("disk full");
        }
        std::lock_guard<std::mutex> lock(mutex);
        frames.insert(frames.end(), packet.begin(), packet.end());
        lost_per_packet.push_back(lost);
    }

    void OnEnd(const SourceEnd& end) override {
        std::lock_guard<std::mutex> lock(mutex);
        ends.push_back(end);
    }

    std::uint64_t TotalLost() const {
        std::lock_guard<std::mutex> lock(mutex);
        return std::accumulate(lost_per_packet.begin(), lost_per_packet.end(), std::uint64_t{0});
    }
};

std::vector<float> Ramp(std::size_t n, float start = 0.0f) {
    std::vector<float> v(n);
    std::iota(v.begin(), v.end(), start);
    return v;
}

}  // namespace

TEST(BufferedSink, DeliversEveryFrameInOrderThenTheEnd) {
    RecordingSink inner;
    {
        BufferedSink sink(inner, 16384);
        const auto audio = Ramp(9600);
        for (std::size_t at = 0; at < audio.size(); at += 160) {
            // One packet reports upstream loss, which must pass through unchanged
            sink.OnAudio(std::span<const float>(audio).subspan(at, 160), at == 1600 ? 3 : 0);
        }
        sink.OnEnd({SourceEndReason::kCompleted, "done"});
        EXPECT_EQ(inner.frames, audio);
        EXPECT_EQ(inner.TotalLost(), 3u);
        ASSERT_EQ(inner.ends.size(), 1u);
        EXPECT_EQ(inner.ends[0].reason, SourceEndReason::kCompleted);
        EXPECT_EQ(inner.ends[0].detail, "done");
    }
    EXPECT_EQ(inner.ends.size(), 1u) << "destruction after an end delivers nothing more";
}

TEST(BufferedSink, AStalledSinkNeverBlocksTheSourceAndOnlyOverrunIsLost) {
    {
        RecordingSink inner;
        inner.hold = true;
        BufferedSink sink(inner, 16000);
        const auto audio = Ramp(8000);
        // The source keeps delivering while the sink is stuck, filling half the ring
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t at = 0; at < audio.size(); at += 160) {
            sink.OnAudio(std::span<const float>(audio).subspan(at, 160), 0);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        inner.hold = false;
        sink.OnEnd({SourceEndReason::kStopped, ""});
        EXPECT_LT(elapsed, std::chrono::milliseconds(50)) << "a held sink must not slow the source";
        EXPECT_EQ(inner.frames, audio) << "a stall within the ring costs nothing";
        EXPECT_EQ(inner.TotalLost(), 0u);
    }

    RecordingSink inner;
    inner.hold = true;
    BufferedSink sink(inner, 1024);
    const auto audio = Ramp(3000);
    for (std::size_t at = 0; at < audio.size(); at += 100) {
        sink.OnAudio(std::span<const float>(audio).subspan(at, 100), 0);
    }
    inner.hold = false;
    sink.OnEnd({SourceEndReason::kStopped, ""});
    // Frames that fitted arrive in order, and the rest must be counted as lost
    ASSERT_LE(inner.frames.size(), 1024u);
    EXPECT_EQ(inner.frames, Ramp(inner.frames.size()));
    EXPECT_EQ(inner.frames.size() + inner.TotalLost(), 3000u);
}

TEST(BufferedSink, TheSinkThrowingEndsTheStreamOnceAsAFailure) {
    RecordingSink inner;
    BufferedSink sink(inner, 1024);
    const auto audio = Ramp(160);
    sink.OnAudio(audio, 0);
    inner.throw_next = true;
    sink.OnAudio(audio, 0);
    sink.OnAudio(audio, 0);
    sink.OnEnd({SourceEndReason::kStopped, ""});
    ASSERT_EQ(inner.ends.size(), 1u);
    EXPECT_EQ(inner.ends[0].reason, SourceEndReason::kFailed);
    EXPECT_NE(inner.ends[0].detail.find("disk full"), std::string::npos);
}

}  // namespace clinicavt::audio
