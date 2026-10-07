#include "core/diarisation/diar_regions.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace clinicavt::diar {
namespace {

constexpr std::uint64_t kHop = audio::kVadHopFrames;

TEST(SpeechRegions, HysteresisPadsSplitsMergesAndDropsBlips) {
    struct Case {
        std::string name;
        std::vector<std::pair<std::size_t, float>> runs;  // hops at one probability
        std::vector<Region> expected;
    };
    const std::vector<Case> cases = {
        {"a spoken stretch becomes one padded region",
         {{10, 0.05f}, {20, 0.90f}, {10, 0.05f}},
         {{10 * kHop - kPadFrames, 30 * kHop + kPadFrames}}},
        // 3 hops are 1536 frames, under the 1600 minimum
        {"a blip shorter than min speech is dropped", {{10, 0.05f}, {3, 0.90f}, {20, 0.05f}}, {}},
        // 4 hops are 2048 frames, under the 2400 minimum
        {"a gap shorter than min silence does not split",
         {{20, 0.90f}, {4, 0.05f}, {20, 0.90f}, {10, 0.05f}},
         {{0, 44 * kHop + kPadFrames}}},
        {"a gap past min silence splits and both edges pad",
         {{20, 0.90f}, {10, 0.05f}, {20, 0.90f}, {10, 0.05f}},
         {{0, 20 * kHop + kPadFrames}, {30 * kHop - kPadFrames, 50 * kHop + kPadFrames}}},
        {"open speech at the end closes at the total",
         {{5, 0.05f}, {20, 0.90f}},
         {{5 * kHop - kPadFrames, 25 * kHop}}},
        // 0.30 is between exit (0.25) and enter (0.40), so it holds a region open but cannot
        // open one
        {"a middling probability does not open a region",
         {{10, 0.05f}, {20, 0.30f}, {10, 0.05f}},
         {}},
        {"a middling probability holds an open region",
         {{10, 0.05f}, {20, 0.90f}, {20, 0.30f}, {10, 0.05f}},
         {{10 * kHop - kPadFrames, 50 * kHop + kPadFrames}}},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        std::vector<float> probs;
        for (const auto& [hops, p] : c.runs) probs.insert(probs.end(), hops, p);
        const auto regions = SpeechRegions(probs, probs.size() * kHop);
        ASSERT_EQ(regions.size(), c.expected.size());
        for (std::size_t i = 0; i < regions.size(); ++i) {
            EXPECT_EQ(regions[i].first_frame, c.expected[i].first_frame) << "region " << i;
            EXPECT_EQ(regions[i].end_frame, c.expected[i].end_frame) << "region " << i;
        }
    }
}

}  // namespace
}  // namespace clinicavt::diar
