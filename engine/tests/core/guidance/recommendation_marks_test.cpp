#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/guidance/document_units.hpp"

namespace clinicavt::guidance {
namespace {

std::vector<Paragraph> Paras(const std::vector<std::string>& texts) {
    std::vector<Paragraph> out;
    for (const auto& text : texts) out.push_back({0, text, {}, {}});
    return out;
}

TEST(RecommendationMarks, EachSchemeReadsItsOwnMarkAndADocumentTakesItsCommonest) {
    EXPECT_EQ(MarkOf("(iv) Offer allopurinol.", Scheme::kRoman), "(iv)");
    EXPECT_EQ(MarkOf("(iv)Offer", Scheme::kRoman), "");
    EXPECT_EQ(MarkOf("(3) Offer", Scheme::kBracketed), "(3)");
    EXPECT_EQ(MarkOf("12. Offer", Scheme::kNumbered), "12.");
    EXPECT_EQ(MarkOf("3.5 mg daily", Scheme::kNumbered), "");
    EXPECT_EQ(MarkOf("1.2.3 Offer", Scheme::kDotted), "1.2.3");
    EXPECT_EQ(MarkOf("Recommendation 2a All people", Scheme::kWord), "Recommendation 2a");
    EXPECT_EQ(MarkOf("recommendation 7", Scheme::kWord), "recommendation 7");
    EXPECT_EQ(MarkOf("R7 Treat early", Scheme::kLetterR), "R7");
    EXPECT_EQ(MarkOf("R33, R39", Scheme::kLetterR), "");
    EXPECT_EQ(MarkOf("(iv) Offer", Scheme::kNone), "");

    EXPECT_EQ(DetectScheme(Paras({"(i) Offer urate lowering therapy.", "Rationale text here.",
                                  "(ii) Start allopurinol at 100 mg."})),
              Scheme::kRoman);
    EXPECT_EQ(DetectScheme(Paras(
                  {"3. Consider methotrexate.", "4. Review at three months.", "(a) sub item"})),
              Scheme::kNumbered);
    EXPECT_EQ(DetectScheme(Paras({"Recommendation 1 All people should be assessed.",
                                  "Recommendation 2a Treat early."})),
              Scheme::kWord);
    EXPECT_EQ(DetectScheme(Paras({"Unchanged recommendations R1-R5, R9-R16", "R33, R39-R41, R46",
                                  "R74, R76, R77"})),
              Scheme::kNone);
    EXPECT_EQ(DetectScheme(Paras({"1.1 Offer allopurinol.", "Body text follows."})), Scheme::kNone);
}

TEST(RecommendationMarks, GradeTokensAndDateTagsCloseARecommendationAndAWrappedTailRejoins) {
    const std::pair<const char*, bool> endings[] = {
        {"All people should be assessed (GRADE 1C, SoA 98%).", true},
        {"LoE: Ib (dose escalation), III. SOR: 97% (range 88-100%).", true},
        {"Continue methotrexate GRADE 2C", true},
        {"Consider stopping (LOE 2++, GOR B, SOA 96%)", true},
        // Strength of evidence closes a recommendation as strength of recommendation does
        {"A single injection is an alternative. LoE: Ib; SOE: 94%.", true},
        {"Refer the person for a spondyloarthritis assessment. [2017]", true},
        {"Offer allopurinol first. [2009, amended 2018]", true},
        {"The GRADE approach to assessing quality was adopted.", false},
        {"The SOR was graded on a 0-100 mm visual analogue scale.", false},
        {"Allopurinol reduces flares in most patients [135]", false},
        {"Treat (GRADE 1B, SoA 90%) and then a long explanation follows in the same paragraph.",
         false},
    };
    for (const auto& [text, ends] : endings) EXPECT_EQ(EndsWithToken(text), ends) << text;

    const std::pair<const char*, bool> tails[] = {
        {"SoA 100%).", true},
        {"SoA 99%).", true},
        {"SOE: 94% (range 83-100%).", true},
        // The recommendation half is not a tail
        {"The decision should be shared with the patient (GRADE 1B,", false},
        {"Offer allopurinol as first-line therapy.", false},
    };
    for (const auto& [text, tail] : tails) EXPECT_EQ(IsGradeTail(text), tail) << text;
}

}  // namespace
}  // namespace clinicavt::guidance
