#include "adapters/models/note_tier.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace clinicavt::models {
namespace {

constexpr std::uint64_t kGb = 1ULL << 30;
const std::vector<std::string> kAll{"constrained", "default", "accuracy"};

// Intel's driver lets the GPU use about three quarters of RAM
MachineMemory Machine(std::uint64_t gb) {
    return {gb * kGb, gb * kGb * 3 / 4};
}

TEST(AutoNoteTier, TakesTheNineBOnATwentyFourGigabyteMachine) {
    EXPECT_EQ(AutoNoteTier(kAll, Machine(64)), "default");
    EXPECT_EQ(AutoNoteTier(kAll, Machine(32)), "default");
    EXPECT_EQ(AutoNoteTier(kAll, Machine(24)), "default");
    EXPECT_EQ(AutoNoteTier(kAll, Machine(16)), "constrained");
}

TEST(AutoNoteTier, AGpuHeldToLessThanTheNineBNeedsStartsSmall) {
    EXPECT_EQ(AutoNoteTier(kAll, {32 * kGb, 8 * kGb}), "constrained");
}

TEST(AutoNoteTier, UnknownMemoryStartsSmall) {
    EXPECT_EQ(AutoNoteTier(kAll, {std::nullopt, 24 * kGb}), "constrained");
    EXPECT_EQ(AutoNoteTier(kAll, {32 * kGb, std::nullopt}), "constrained");
}

TEST(AutoNoteTier, TakesWhatIsStaged) {
    EXPECT_EQ(AutoNoteTier({"constrained"}, Machine(32)), "constrained");
    EXPECT_EQ(AutoNoteTier({"default"}, Machine(16)), "default");
    EXPECT_EQ(AutoNoteTier({"default", "accuracy"}, Machine(16)), "default");
    EXPECT_EQ(AutoNoteTier({"accuracy"}, Machine(16)), "accuracy");
    EXPECT_EQ(AutoNoteTier({}, Machine(32)), "");
}

TEST(AutoNoteTier, NeverPicksTheThirtyFiveBOverAnother) {
    EXPECT_EQ(AutoNoteTier({"constrained", "accuracy"}, Machine(64)), "constrained");
}

}  // namespace
}  // namespace clinicavt::models
