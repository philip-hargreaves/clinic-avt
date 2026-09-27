#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "adapters/diarisation/segmenter.hpp"

namespace clinicavt::diar {
namespace {

// Helpers author class runs: 0 silence, 1-3 one speaker, 4+ overlap
std::vector<std::int8_t> Frames(std::initializer_list<std::pair<int, int>> runs) {
    std::vector<std::int8_t> classes;
    for (const auto& [cls, count] : runs) {
        classes.insert(classes.end(), static_cast<std::size_t>(count),
                       static_cast<std::int8_t>(cls));
    }
    return classes;
}

// Audio frames per class frame when a window holds 200 class frames
std::uint64_t At(int frame) {
    return static_cast<std::uint64_t>(frame * (static_cast<double>(kSegWindowFrames) / 200.0));
}

TEST(SegDecode, ChangePointsNeedAHeldSpeakerAndSkipSilenceAndOverlap) {
    struct Case {
        std::string name;
        std::vector<std::int8_t> classes;
        std::size_t changes;
    };
    const std::vector<Case> cases = {
        {"a flip after the hold", Frames({{1, 100}, {2, 100}}), 1},
        // Speaker 2 for 4 frames only (under the 6-frame hold), then back: the flip TO the
        // flicker is a change because speaker 1 held, the flip back is not
        {"a flicker under the hold", Frames({{1, 100}, {2, 4}, {1, 100}}), 1},
        {"a 4-frame holder", Frames({{1, 4}, {2, 100}}), 0},
        {"silence between one speaker", Frames({{1, 100}, {0, 50}, {1, 100}}), 0},
        {"an overlap handover", Frames({{1, 100}, {4, 20}, {2, 100}}), 1},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        SegResult result;
        DecodeSegWindow(c.classes, 0, result);
        EXPECT_EQ(result.change_points.size(), c.changes);
    }

    SegResult flip;
    DecodeSegWindow(Frames({{1, 100}, {2, 100}}), 0, flip);
    ASSERT_EQ(flip.change_points.size(), 1u);
    EXPECT_EQ(flip.change_points[0], At(100));
}

TEST(SegDecode, OverlapClassesMergeIntoSpansWithinAndAcrossWindows) {
    SegResult within;
    DecodeSegWindow(Frames({{1, 100}, {4, 10}, {5, 10}, {1, 80}}), 0, within);
    ASSERT_EQ(within.overlap_spans.size(), 1u) << "adjacent overlap classes merge into one span";
    EXPECT_EQ(within.overlap_spans[0].first_frame, At(100));
    EXPECT_EQ(within.overlap_spans[0].end_frame, At(120));

    SegResult across;
    DecodeSegWindow(Frames({{1, 195}, {4, 5}}), 0, across);
    DecodeSegWindow(Frames({{4, 5}, {1, 195}}), kSegWindowFrames, across);
    ASSERT_EQ(across.overlap_spans.size(), 1u) << "a span continues over the window edge";
    EXPECT_EQ(across.overlap_spans[0].end_frame, kSegWindowFrames + At(5));
}

}  // namespace
}  // namespace clinicavt::diar
