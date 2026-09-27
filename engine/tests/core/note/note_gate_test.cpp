#include "core/note/note_gate.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace clinicavt::note {
namespace {

TEST(RefusalReason, ReadsTheSentinelLineAndNothingElse) {
    EXPECT_EQ(RefusalReason("NOT A CONSULTATION: a cooking video, one speaker\n"),
              "a cooking video, one speaker");
    EXPECT_EQ(RefusalReason("  \nNOT A CONSULTATION:   a lecture\nmore text"), "a lecture");
    EXPECT_FALSE(RefusalReason("The patient presented with a sore elbow.").has_value());
    EXPECT_FALSE(RefusalReason("Not a consultation of the usual kind, the patient...").has_value())
        << "case matters: the sentinel is exact";
}

TEST(RefusalFilter, HoldsOnlyWhileTheOpeningCouldBeTheSentinel) {
    std::vector<std::string> seen;
    const auto collect = [&seen](const std::string& t) { seen.push_back(t); };

    RefusalFilter ordinary(collect);
    ordinary("The");
    EXPECT_EQ(seen.size(), 1u) << "an ordinary opening streams at once";

    seen.clear();
    RefusalFilter note(collect);
    note("NOT");
    note("NOT A CONS");
    EXPECT_TRUE(seen.empty()) << "could still be a refusal";
    note("NOT A CONSIDERABLE delay, the patient presented");
    ASSERT_EQ(seen.size(), 1u) << "decided: not the sentinel";
    note("NOT A CONSIDERABLE delay, the patient presented with");
    EXPECT_EQ(seen.size(), 2u);
    EXPECT_FALSE(note.Refused());

    seen.clear();
    RefusalFilter refusal(collect);
    refusal("NOT A CONSULTATION:");
    refusal("NOT A CONSULTATION: a ramen video");
    EXPECT_TRUE(seen.empty()) << "a refusal is swallowed whole";
    EXPECT_TRUE(refusal.Refused());
}

}  // namespace
}  // namespace clinicavt::note
