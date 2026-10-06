#include "core/audio/audio_ring.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <thread>
#include <vector>

namespace clinicavt::audio {
namespace {

std::vector<float> Sequence(std::size_t count, float first = 0.0F) {
    std::vector<float> frames(count);
    for (std::size_t i = 0; i < count; ++i) {
        frames[i] = first + static_cast<float>(i);
    }
    return frames;
}

TEST(AudioRing, PushesWhatFitsAndPopsInOrderAcrossTheWrap) {
    AudioRing ring(8);
    std::vector<float> out(10);
    EXPECT_EQ(ring.TryPop(out), 0u) << "an empty ring pops nothing";

    EXPECT_EQ(ring.TryPush(Sequence(10)), 8u) << "a push beyond capacity writes only what fits";
    ASSERT_EQ(ring.TryPop(out), 8u);
    EXPECT_EQ(std::vector<float>(out.begin(), out.begin() + 8), Sequence(8));

    EXPECT_EQ(ring.TryPush(Sequence(8, 100.0F)), 8u) << "popping frees the space";
    ASSERT_EQ(ring.TryPop(out), 8u);
    EXPECT_EQ(std::vector<float>(out.begin(), out.begin() + 8), Sequence(8, 100.0F));

    // The second push crosses the physical end of the buffer
    EXPECT_EQ(ring.TryPush(Sequence(6)), 6u);
    ASSERT_EQ(ring.TryPop(std::span<float>(out).first(6)), 6u);
    EXPECT_EQ(ring.TryPush(Sequence(6, 50.0F)), 6u);
    ASSERT_EQ(ring.TryPop(out), 6u);
    EXPECT_EQ(std::vector<float>(out.begin(), out.begin() + 6), Sequence(6, 50.0F));
}

constexpr std::size_t kTotalFrames = 1 << 20;  // Exact as a float, under 2^24
constexpr std::size_t kChunk = 480;

// gtest assertions are not thread-safe on Windows, so the threads only count
// and every assertion happens after the join
TEST(AudioRing, TwoThreadsMoveEveryFrameInOrder) {
    AudioRing ring(1024);

    std::thread producer([&ring] {
        std::vector<float> chunk(kChunk);
        std::size_t sent = 0;
        while (sent < kTotalFrames) {
            const std::size_t count = std::min(kChunk, kTotalFrames - sent);
            for (std::size_t i = 0; i < count; ++i) {
                chunk[i] = static_cast<float>(sent + i);
            }
            std::span<const float> remaining(chunk.data(), count);
            while (!remaining.empty()) {
                remaining = remaining.subspan(ring.TryPush(remaining));
            }
            sent += count;
        }
    });

    std::size_t received = 0;
    std::size_t out_of_sequence = 0;
    std::vector<float> out(512);
    while (received < kTotalFrames) {
        const std::size_t count = ring.TryPop(out);
        for (std::size_t i = 0; i < count; ++i) {
            if (out[i] != static_cast<float>(received + i)) {
                ++out_of_sequence;
            }
        }
        received += count;
    }
    producer.join();

    EXPECT_EQ(received, kTotalFrames);
    EXPECT_EQ(out_of_sequence, 0u);
    EXPECT_EQ(ring.TryPop(out), 0u);
}

}  // namespace
}  // namespace clinicavt::audio
