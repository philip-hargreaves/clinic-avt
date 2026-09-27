#include "core/diarisation/slice_refinement.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace clinicavt::diar {
namespace {

std::vector<Region> R(std::initializer_list<Region> regions) {
    return regions;
}

void ExpectRegions(const std::vector<Region>& got, const std::vector<Region>& want) {
    ASSERT_EQ(got.size(), want.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        EXPECT_EQ(got[i].first_frame, want[i].first_frame) << "range " << i;
        EXPECT_EQ(got[i].end_frame, want[i].end_frame) << "range " << i;
    }
}

TEST(RefineRegions, SplitsAtInteriorCutsSnapsEdgesAndDropsFragments) {
    struct Case {
        std::string name;
        std::vector<Region> regions;
        std::vector<std::uint64_t> cuts;
        std::vector<Region> slices;
    };
    const std::vector<Case> cases = {
        {"a change point inside a region splits it",
         R({{10000, 50000}}),
         {30000},
         R({{10000, 30000}, {30000, 50000}})},
        {"a cut within the left edge margin is the edge",
         R({{10000, 50000}}),
         {10500},
         R({{10000, 50000}})},
        {"a cut within the right edge margin is the edge",
         R({{10000, 50000}}),
         {49500},
         R({{10000, 50000}})},
        // Past the margin the cut registers and its sub-minimum left fragment drops
        {"a cut just past the edge margin", R({{10000, 50000}}), {10801}, R({{10801, 50000}})},
        // A 2000-frame fragment on the left, under the 3200 floor
        {"a sub-minimum fragment drops and the rest survives",
         R({{10000, 50000}}),
         {12000},
         R({{12000, 50000}})},
        {"cuts outside a region are ignored",
         R({{10000, 50000}, {60000, 90000}}),
         {55000, 70000},
         R({{10000, 50000}, {60000, 70000}, {70000, 90000}})},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        ExpectRegions(RefineRegions(c.regions, c.cuts), c.slices);
    }
}

TEST(EmbeddingRanges, ExcludeOverlapOnlyWhenEnoughCleanAudioRemains) {
    struct Case {
        std::string name;
        Region slice;
        std::vector<Region> overlap;
        std::vector<Region> ranges;
    };
    const std::vector<Case> cases = {
        {"overlap is cut out when enough clean remains",
         {0, 20000},
         R({{8000, 10000}}),
         R({{0, 8000}, {10000, 20000}})},
        // 7000 of 10000 frames overlapped: 3000 clean, under the 8000 minimum
        {"a mostly overlapped slice embeds whole", {0, 10000}, R({{3000, 10000}}), R({{0, 10000}})},
        {"a slice under the floor does not embed", {0, 3000}, R({{0, 2000}}), {}},
        {"no overlap is the whole slice as one range", {5000, 25000}, {}, R({{5000, 25000}})},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        ExpectRegions(EmbeddingRanges(c.slice, c.overlap), c.ranges);
    }
}

}  // namespace
}  // namespace clinicavt::diar
