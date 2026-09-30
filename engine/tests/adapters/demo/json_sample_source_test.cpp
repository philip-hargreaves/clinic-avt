#include "adapters/demo/json_sample_source.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>

#include "core/note/summary_scrub.hpp"

namespace clinicavt::demo {
namespace {

// Also checks the scrub would change nothing the app shows
TEST(SampleYear, TheShippedContentIsWholeAndSpreadOverTheYear) {
    const auto samples = JsonSampleSource(CLINICAVT_DEMO_DIR).Load();
    ASSERT_EQ(samples.size(), 6u);
    std::set<int> months;
    std::set<std::string> sources;
    for (const auto& s : samples) {
        months.insert(s.months_back);
        sources.insert(s.source);
        EXPECT_GE(s.months_back, 0) << s.source;
        EXPECT_LE(s.months_back, 12) << s.source;
        EXPECT_FALSE(s.label.empty()) << s.source;
        EXPECT_FALSE(s.note.empty()) << s.source;
        EXPECT_FALSE(s.patient.empty()) << s.source;
        EXPECT_FALSE(s.summary.empty()) << s.source;
        EXPECT_GT(s.turns.size(), 50u) << s.source;
        EXPECT_GT(s.audio_seconds, 300.0) << s.source;
        EXPECT_FALSE(s.happened.empty()) << s.source;
        EXPECT_FALSE(s.learned.empty()) << s.source;
        EXPECT_FALSE(s.next.empty()) << s.source;
        for (const std::string* text : {&s.summary, &s.happened, &s.learned, &s.next}) {
            EXPECT_EQ(note::ScrubSummary(*text), *text) << s.source;
        }
        std::uint64_t last_start = 0;
        for (const auto& turn : s.turns) {
            EXPECT_GE(turn.first_frame, last_start) << s.source;
            EXPECT_TRUE(turn.speaker == "doctor" || turn.speaker == "patient") << s.source;
            last_start = turn.first_frame;
        }
    }
    EXPECT_EQ(months.size(), 5u) << "two in the current month, the rest a month each";
    EXPECT_TRUE(months.contains(0)) << "the current month shows samples";
    EXPECT_EQ(sources.size(), 6u) << "a consultation used twice";
}

}  // namespace
}  // namespace clinicavt::demo
