#include "core/diarisation/clip_cuts.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace clinicavt::diar {
namespace {

// 32 ms hops. Speech everywhere except a pause at hops 40-42 (1.28-1.34 s)
std::vector<float> Probabilities() {
    std::vector<float> p(200, 0.9f);
    p[40] = 0.3f;
    p[41] = 0.05f;
    p[42] = 0.3f;
    return p;
}

TEST(SnapClipCuts, EdgesSnapOntoAPauseOrAreDropped) {
    struct Case {
        std::string name;
        std::vector<std::uint64_t> cuts;
        std::vector<std::uint64_t> seg;
        std::vector<std::uint64_t> kept;
    };
    const std::vector<Case> cases = {
        {"an edge 190 ms after the pause moves onto it", {41 * 512 + 3000}, {}, {41 * 512}},
        {"an edge with no pause nearby is dropped", {120 * 512}, {}, {}},
        {"an edge 250 ms from a segmenter cut is dropped", {41 * 512}, {41 * 512 + 4000}, {}},
        {"two edges on one pause keep one", {41 * 512 - 2000, 41 * 512 + 2000}, {}, {41 * 512}},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        EXPECT_EQ(SnapClipCuts(c.cuts, Probabilities(), c.seg), c.kept);
    }
}

}  // namespace
}  // namespace clinicavt::diar
