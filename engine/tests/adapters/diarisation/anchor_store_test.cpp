#include "adapters/diarisation/anchor_store.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "adapters/diarisation/cluster_voiceprint.hpp"

namespace clinicavt::diar {
namespace {

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("clinicavt-anchor-" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::vector<float> Unit(float x, float y) {
    const float norm = std::sqrt(x * x + y * y);
    return {x / norm, y / norm};
}

TEST(AnchorStore, AccrualThenEnrolmentThenRefinementTracksOrigin) {
    TempDir dir;
    AnchorStore store(dir.path);
    EXPECT_FALSE(store.Anchor().has_value());
    EXPECT_EQ(store.Status().origin, AnchorOrigin::kNone);

    store.Accrue(Unit(1, 0));
    store.Accrue(Unit(0, 1));
    EXPECT_EQ(store.Status().origin, AnchorOrigin::kAccrued);
    EXPECT_EQ(store.Status().sessions, 2u);
    auto anchor = *store.Anchor();
    EXPECT_NEAR(anchor[0], anchor[1], 1e-6) << "the mean of two orthogonal units is diagonal";
    EXPECT_NEAR(anchor[0] * anchor[0] + anchor[1] * anchor[1], 1.0, 1e-5) << "unit norm";

    // An enrolment discards what accrued before it
    store.Replace(Unit(1, 0), 1'757'000'000);
    auto status = store.Status();
    EXPECT_EQ(status.origin, AnchorOrigin::kEnrolled);
    EXPECT_EQ(status.sessions, 0u);
    EXPECT_EQ(status.enrolled_at, 1'757'000'000u);
    EXPECT_NEAR((*store.Anchor())[0], 1.0f, 1e-5);

    store.Accrue(Unit(0, 1));
    status = store.Status();
    EXPECT_EQ(status.origin, AnchorOrigin::kEnrolled) << "refined, still enrolled";
    EXPECT_EQ(status.sessions, 1u);
    anchor = *store.Anchor();
    EXPECT_GT(anchor[0], anchor[1]) << "the enrolment outweighs one consultation";
    EXPECT_GT(anchor[1], 0.0f) << "but the consultation moved it";
}

TEST(AnchorStore, PersistsAcrossReloadAndClearErasesTheFile) {
    TempDir dir;
    {
        AnchorStore store(dir.path);
        store.Replace(Unit(3, 4), 42);
        store.Accrue(Unit(3, 4));
    }
    {
        AnchorStore reloaded(dir.path);
        const auto status = reloaded.Status();
        EXPECT_EQ(status.origin, AnchorOrigin::kEnrolled);
        EXPECT_EQ(status.sessions, 1u);
        EXPECT_EQ(status.enrolled_at, 42u);
        ASSERT_TRUE(reloaded.Anchor().has_value());
        EXPECT_NEAR((*reloaded.Anchor())[0], 0.6f, 1e-5);

        // Forgetting the voiceprint must remove it from disk, not just from memory
        reloaded.Clear();
        EXPECT_FALSE(reloaded.Anchor().has_value());
        EXPECT_EQ(reloaded.Status().origin, AnchorOrigin::kNone);
        EXPECT_EQ(reloaded.Status().enrolled_at, 0u);
    }
    EXPECT_FALSE(std::filesystem::exists(dir.path / "anchor.bin"));
    AnchorStore reloaded(dir.path);
    EXPECT_FALSE(reloaded.Anchor().has_value());
}

TEST(AnchorStore, ACorruptFileResetsToEmpty) {
    TempDir dir;
    std::ofstream(dir.path / "anchor.bin", std::ios::binary) << "not a wrapped blob";
    AnchorStore store(dir.path);
    EXPECT_FALSE(store.Anchor().has_value());
    store.Accrue(Unit(1, 0));  // and it can accrue again afterwards
    EXPECT_EQ(store.Status().sessions, 1u);
}

TEST(AnchorRecord, RoundTripsAndRefusesAnotherVersion) {
    const detail::AnchorRecord record{{0.6f, 0.8f}, 5, 42};
    auto plain = detail::SerializeAnchor(record);
    const auto again = detail::ParseAnchor(plain);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->sum, record.sum);
    EXPECT_EQ(again->sessions, 5u);
    EXPECT_EQ(again->enrolled_at, 42u);

    const std::uint32_t version = 1;
    std::memcpy(plain.data(), &version, 4);
    EXPECT_FALSE(detail::ParseAnchor(plain).has_value()) << "another version starts fresh";
    EXPECT_FALSE(detail::ParseAnchor(std::vector<std::uint8_t>(10)).has_value()) << "too short";
}

TEST(VoiceprintRanges, StopAtTheCapAndRefuseUnderASecond) {
    std::vector<LabelledSlice> slices;
    for (int i = 0; i < 5; ++i) {  // five 40 s slices of cluster 0
        const auto start = static_cast<std::uint64_t>(i) * 700000;
        slices.push_back({start, start + 640000, 0});
    }
    slices.push_back({4000000, 4008000, 1});  // cluster 1: half a second only

    const auto ranges = VoiceprintRanges(slices, 0);
    ASSERT_EQ(ranges.size(), 3u) << "the slice crossing 90 s is kept whole, then stop";
    EXPECT_TRUE(VoiceprintRanges(slices, 1).empty()) << "under a second carries no identity";
}

}  // namespace
}  // namespace clinicavt::diar
