#include "adapters/models/note_tier.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace clinicavt::models {
namespace {

constexpr std::uint64_t kGb = 1ULL << 30;

// Intel's driver lets the GPU use about three quarters of RAM
MachineMemory Machine(std::uint64_t gb) {
    return {gb * kGb, gb * kGb * 3 / 4};
}

TEST(AutoNoteTier, StartsOnTheNineBWhenItFitsElseTheSmallestStagedAndNeverTheThirtyFiveB) {
    const std::vector<std::string> all{"constrained", "default", "accuracy"};
    struct Row {
        const char* why;
        std::vector<std::string> staged;
        MachineMemory memory;
        std::string tier;
    };
    const Row rows[] = {
        {"64 GB", all, Machine(64), "default"},
        {"24 GB is enough for the 9B", all, Machine(24), "default"},
        {"16 GB is not", all, Machine(16), "constrained"},
        {"a GPU held under what the 9B needs", all, {32 * kGb, 8 * kGb}, "constrained"},
        {"unknown RAM", all, {std::nullopt, 24 * kGb}, "constrained"},
        {"unknown GPU memory", all, {32 * kGb, std::nullopt}, "constrained"},
        {"only the 4B staged", {"constrained"}, Machine(32), "constrained"},
        {"only the 9B staged, too big or not", {"default"}, Machine(16), "default"},
        {"the 9B before the 35B", {"default", "accuracy"}, Machine(16), "default"},
        {"only the 35B staged", {"accuracy"}, Machine(16), "accuracy"},
        {"the 4B before the 35B on any machine",
         {"constrained", "accuracy"},
         Machine(64),
         "constrained"},
        {"nothing staged", {}, Machine(32), ""},
    };
    for (const Row& row : rows)
        EXPECT_EQ(AutoNoteTier(row.staged, row.memory), row.tier) << row.why;
}

}  // namespace
}  // namespace clinicavt::models
