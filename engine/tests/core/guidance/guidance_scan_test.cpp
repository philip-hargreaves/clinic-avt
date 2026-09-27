#include "core/guidance/guidance_scan.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace clinicavt::guidance {
namespace {

TEST(Scan, ReturnsTheNearestRowsWithTiesOnOrdinalAndKClamped) {
    // Four unit vectors in three dimensions
    const std::vector<float> matrix = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0.6f, 0.8f, 0};

    const float near[] = {0.6f, 0.8f, 0};
    const auto hits = Scan(matrix.data(), 4, 3, near, 3);
    ASSERT_EQ(hits.size(), 3u);
    EXPECT_EQ(hits[0].ord, 3u);
    EXPECT_NEAR(hits[0].cosine, 1.0f, 1e-6f);
    EXPECT_EQ(hits[1].ord, 1u);
    EXPECT_NEAR(hits[1].cosine, 0.8f, 1e-6f);
    EXPECT_EQ(hits[2].ord, 0u);
    EXPECT_NEAR(hits[2].cosine, 0.6f, 1e-6f);

    const float axis[] = {0, 0, 1};
    EXPECT_EQ(Scan(matrix.data(), 4, 3, axis, 10).size(), 4u);
    EXPECT_TRUE(Scan(matrix.data(), 4, 3, axis, 0).empty());
    const auto tied = Scan(matrix.data(), 4, 3, axis, 4);
    EXPECT_EQ(tied[0].ord, 2u);
    EXPECT_EQ(tied[1].ord, 0u) << "rows 0, 1 and 3 all score 0; lowest ordinal first";
    EXPECT_EQ(tied[2].ord, 1u);
    EXPECT_EQ(tied[3].ord, 3u);

    EXPECT_TRUE(Scan(matrix.data(), 0, 3, near, 5).empty());
}

}  // namespace
}  // namespace clinicavt::guidance
