#include "core/diarisation/frontier.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace clinicavt::diar {
namespace {

TEST(SegSettledFrontier, TrailsTheSlowerSideAndSettlesNothingInTheFirstMargin) {
    EXPECT_EQ(SegSettledFrontier(320000, 160000), 160000u - kSegFrontierMarginFrames)
        << "vad behind: trails it by the region margin";
    EXPECT_EQ(SegSettledFrontier(100000, 320000), 100000u) << "seg behind";
    EXPECT_EQ(SegSettledFrontier(320000, kSegFrontierMarginFrames), 0u)
        << "inside the first margin";
    EXPECT_EQ(SegSettledFrontier(320000, 0), 0u) << "no underflow at the start";
}

// 32 ms hops: speech to hop 50, silence after
std::vector<float> SpeechThenSilence(std::size_t hops) {
    std::vector<float> p(hops, 0.05f);
    for (std::size_t i = 0; i <= 50 && i < hops; ++i) p[i] = 0.9f;
    return p;
}

TEST(TurnClosed, ClosesOnlyAfterAFullSilentWindow) {
    const auto end = 51u * audio::kVadHopFrames;
    EXPECT_TRUE(TurnClosed(SpeechThenSilence(120), end)) << "silence after the end closes the turn";
    EXPECT_FALSE(TurnClosed(SpeechThenSilence(60), end))
        << "not closed while the audio is shorter than the silence window";
    auto blip = SpeechThenSilence(120);
    blip[65] = 0.8f;
    EXPECT_FALSE(TurnClosed(blip, end)) << "speech inside the window keeps it open";
}

}  // namespace
}  // namespace clinicavt::diar
