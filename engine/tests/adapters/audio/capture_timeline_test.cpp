#include "adapters/audio/capture_timeline.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

namespace clinicavt::audio {
namespace {

// 48 kHz native, 16 kHz target, 10 ms packets
constexpr std::uint32_t kNative = 48000;
constexpr std::uint32_t kFrames = 160;              // Target frames per packet
constexpr std::uint64_t kNativeStep = kFrames * 3;  // Native frames per packet

TEST(CaptureTimeline, AGapIsCountedOnceInTargetFrames) {
    CaptureTimeline timeline(kNative);

    // Arbitrary origin
    constexpr std::uint64_t kOrigin = 987654;
    EXPECT_EQ(timeline.OnPacket(kOrigin, kFrames), 0u) << "the first packet anchors";

    std::uint64_t packet = 1;
    for (; packet < 100; ++packet) {
        EXPECT_EQ(timeline.OnPacket(kOrigin + packet * kNativeStep, kFrames), 0u)
            << "clean packet " << packet;
    }

    // 4800 native frames go missing, 100 ms or 1600 target frames
    constexpr std::uint64_t kGap = 4800;
    EXPECT_EQ(timeline.OnPacket(kOrigin + packet++ * kNativeStep + kGap, kFrames), 1600u);

    // Positions keep the same offset forever, and the loss is not reported again
    for (; packet < 150; ++packet) {
        EXPECT_EQ(timeline.OnPacket(kOrigin + packet * kNativeStep + kGap, kFrames), 0u)
            << "packet " << packet << " after the gap";
    }

    // Native equal to target is the identity case
    CaptureTimeline identity(16000);
    identity.OnPacket(0, kFrames);
    EXPECT_EQ(identity.OnPacket(kFrames + 320, kFrames), 320u);
}

TEST(CaptureTimeline, RoundingAndBackwardPositionsNeverInventLoss) {
    // 44.1 kHz native with 100-frame packets, so every position is rounded from a
    // non-integral value and has sub-frame wobble
    CaptureTimeline rounding(44100);
    double ideal = 0.0;
    for (int packet = 0; packet < 2000; ++packet) {
        const auto position = static_cast<std::uint64_t>(std::llround(ideal));
        EXPECT_EQ(rounding.OnPacket(position, 100), 0u) << "packet " << packet;
        ideal += 100.0 * 44100.0 / 16000.0;
    }

    CaptureTimeline timeline(kNative);
    timeline.OnPacket(0, kFrames);
    timeline.OnPacket(kNativeStep, kFrames);
    // A device hiccup reports an earlier position, which is not loss
    EXPECT_EQ(timeline.OnPacket(kNativeStep / 2, kFrames), 0u);
    // No loss is reported when positions resume where they should be
    EXPECT_EQ(timeline.OnPacket(3 * kNativeStep, kFrames), 0u);
}

}  // namespace
}  // namespace clinicavt::audio
