#include "core/guidance/guidance_query.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace clinicavt::guidance {
namespace {

TEST(SplitSentences, MirrorsTheHarnessBoundaries) {
    struct Case {
        const char* note;
        std::vector<std::string> sentences;
        const char* why;
    };
    const Case cases[] = {
        {"A 42-year-old woman presents with six weeks of pain. Examination shows synovitis of "
         "several MCP joints. Bloods show a normal CRP.\nPlan: urgent referral to rheumatology.",
         {"A 42-year-old woman presents with six weeks of pain.",
          "Examination shows synovitis of several MCP joints.", "Bloods show a normal CRP.",
          "Plan: urgent referral to rheumatology."},
         "full stops and line breaks end sentences"},
        {"Seen by Dr. Patel with the pt. today. Takes 5 mg. Bd for pain, e.g. After meals. "
         "Reviewed by J. Smith yesterday.",
         {"Seen by Dr. Patel with the pt. today.", "Takes 5 mg. Bd for pain, e.g. After meals.",
          "Reviewed by J. Smith yesterday."},
         "abbreviations and initials do not end a sentence"},
        {"Pain for 3 days. then eased. OK.\nNil else. Plan as above today.",
         {"Pain for 3 days. then eased.", "Plan as above today."},
         "a lowercase word continues; OK. and Nil else. are under three words"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.why);
        EXPECT_EQ(SplitSentences(c.note), c.sentences);
    }
}

TEST(IsExcluded, NegationsHistoryAndHypotheticalsAreOutFindingsAndPlansStayIn) {
    const std::pair<const char*, bool> cases[] = {
        {"No headache, no visual disturbance, no neurological symptoms.", true},
        {"Nil frequency.", true},
        {"Denies chest pain or shortness of breath.", true},
        {"FH: mother had type 2 diabetes.", true},
        {"Her mother had breast cancer at 52.", true},
        {"If symptoms worsen she should attend the emergency department.", true},
        {"Not pregnant.", true},
        {"Presents with a two-day history of dysuria and frequency.", false},
        {"Plan: back-up antibiotic prescription and self-care advice.", false},
        {"Examination shows synovitis of several MCP joints.", false},
        {"Known hypertension, on amlodipine.", false},
    };
    for (const auto& [sentence, excluded] : cases)
        EXPECT_EQ(IsExcluded(sentence), excluded) << sentence;
}

TEST(SubQueries, FilteredSentencesThenTheWholeNote) {
    const std::string note =
        "No headache, no visual disturbance, no neurological symptoms. Presents with a two-day "
        "history of dysuria and frequency. Not pregnant. Plan: back-up antibiotic prescription and "
        "self-care advice.";
    const auto queries = SubQueries(note);
    ASSERT_EQ(queries.size(), 3u);
    EXPECT_EQ(queries[0], "Presents with a two-day history of dysuria and frequency.");
    EXPECT_EQ(queries[1], "Plan: back-up antibiotic prescription and self-care advice.");
    EXPECT_EQ(queries[2], note) << "the whole note is the last sub-query";

    EXPECT_EQ(SubQueries("Presents with a two-day history of dysuria.").size(), 1u)
        << "a one-sentence note is one query";
    EXPECT_TRUE(SubQueries("Minutes.").empty());
}

}  // namespace
}  // namespace clinicavt::guidance
