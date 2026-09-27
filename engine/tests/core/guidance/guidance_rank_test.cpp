#include "core/guidance/guidance_rank.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace clinicavt::guidance {
namespace {

// Three sub-queries, hand-computed reciprocal rank fusion with k = 60:
//   y: 1/62 + 1/61 + 1/62   x: 1/61 + 1/62   z: 1/63 + 1/61
std::vector<SubQueryHits> Lists() {
    return {{"sentence one", false, {{"x", 0.90}, {"y", 0.88}, {"z", 0.80}}},
            {"sentence two", false, {{"y", 0.91}, {"x", 0.86}}},
            {"the whole note", true, {{"z", 0.87}, {"y", 0.84}}}};
}

TEST(RankVote, ReproducesTheHarnessFusionWithBestCosineAndTrigger) {
    const auto ranked = RankVote(Lists());
    ASSERT_EQ(ranked.size(), 3u);
    EXPECT_EQ(ranked[0].id, "y");
    EXPECT_EQ(ranked[1].id, "x");
    EXPECT_EQ(ranked[2].id, "z");
    EXPECT_NEAR(ranked[0].score, 1.0 / 62 + 1.0 / 61 + 1.0 / 62, 1e-12);
    EXPECT_NEAR(ranked[1].score, 1.0 / 61 + 1.0 / 62, 1e-12);
    EXPECT_NEAR(ranked[2].score, 1.0 / 63 + 1.0 / 61, 1e-12);

    // Each candidate keeps its best cosine and the query that ranked it highest
    EXPECT_DOUBLE_EQ(ranked[0].cosine, 0.91);
    EXPECT_EQ(ranked[0].trigger, "sentence two");
    EXPECT_EQ(ranked[1].trigger, "sentence one");
    EXPECT_EQ(ranked[2].trigger, "the whole note");

    std::vector<SubQueryHits> many{{"q", false, {}}};
    for (int i = 0; i < 80; ++i) many[0].hits.push_back({"c" + std::to_string(i), 0.9});
    EXPECT_EQ(RankVote(many).size(), static_cast<std::size_t>(kUnionSize));
    EXPECT_EQ(RankVote(many, 10).size(), 10u);
}

TEST(RankVote, AHitUnderTheVoteFloorCastsNoVote) {
    std::vector<SubQueryHits> lists{{"sentence", false, {{"a", 0.90}, {"b", 0.80}}},
                                    {"note", true, {{"b", 0.88}, {"c", 0.70}}}};
    const auto ranked = RankVote(lists, kUnionSize, 0.85);
    ASSERT_EQ(ranked.size(), 3u);
    EXPECT_EQ(ranked[0].id, "a");
    EXPECT_EQ(ranked[1].id, "b");
    EXPECT_DOUBLE_EQ(ranked[1].score, 1.0 / (kRrfK + 1.0)) << "only the note's vote counts";
    EXPECT_EQ(ranked[2].id, "c") << "the whole note votes under the floor";
}

TEST(ApplyFloor, TheFloorDropsWeakHitsAndAbstainsWhenNothingRemains) {
    const auto kept = ApplyFloor(RankVote(Lists()), 0.875);
    ASSERT_EQ(kept.kept.size(), 2u) << "z's best cosine is 0.87, under the floor";
    EXPECT_EQ(kept.kept[0].id, "y");
    EXPECT_EQ(kept.kept[1].id, "x");
    EXPECT_EQ(kept.considered, 3);
    EXPECT_FALSE(kept.abstained);
    const auto none = ApplyFloor(RankVote(Lists()), 0.95);
    EXPECT_TRUE(none.abstained);
    EXPECT_TRUE(none.kept.empty());
    EXPECT_EQ(none.considered, 3);

    // Only the whole-note list decides whether a note clears the floor
    std::vector<SubQueryHits> lists{{"sentence", false, {{"a", 0.95}}},
                                    {"note", true, {{"b", 0.84}}}};
    EXPECT_FALSE(NoteClears(lists, 0.85));
    EXPECT_TRUE(NoteClears(lists, 0.84));
    lists.pop_back();
    EXPECT_TRUE(NoteClears(lists, 0.85)) << "no whole-note list leaves it to the floor";
}

TEST(PopulationConflict, AnotherPopulationConflictsOnlyWhenTheNoteOrTitleIsExplicit) {
    const char* const uti =
        "A 19-year-old woman, not pregnant, reports three days of dysuria. Plan: three-day course.";
    const char* const adult = "53-year-old male with two days of vomiting and diarrhoea.";
    const char* const gastro =
        "Suspect gastroenteritis if there is a sudden change in stool consistency.";
    const char* const children = "Refer children with recurrent infection.";
    struct Case {
        const char* note;
        const char* recommendation;
        const char* title;
        bool conflict;
        const char* why;
    };
    const Case cases[] = {
        {uti, "Offer an immediate antibiotic to pregnant women.", "", true, "not pregnant"},
        {uti, "Consider a back-up prescription for women who are not pregnant.", "", false,
         "not pregnant agrees"},
        {uti, "When prescribing for a man, choose a seven-day course.", "", true,
         "the note says woman"},
        {uti, children, "", true, "a 19-year-old is not a child"},
        {"Dysuria for three days. Plan: antibiotics.",
         "Offer an immediate antibiotic to pregnant women.", "", false,
         "silence on pregnancy is not a contradiction"},
        {"A 9-year-old child with fever.", children, "", false, "a child"},
        {"Patient aged 72 with new back pain.", children, "", true, "aged 72"},
        // The title counts, and sex stated alone says adult
        {adult, gastro, "Diarrhoea and vomiting caused by gastroenteritis in under 5s", true,
         "the title names under 5s"},
        {adult, gastro, "Gastroenteritis in adults", false, "the title names adults"},
        {"Male patient with dysuria.", children, "", true,
         "sex stated, no age, no child: an adult"},
        {"Male, 9 years old, with dysuria.", children, "", false, "male aged 9"},
        {"Male child with dysuria.", children, "", false, "male child"},
        {adult, "Offer a test to people under 50 with weight loss.",
         "Type 1 diabetes in adults: diagnosis and management", false,
         "an adult threshold is not a child"},
        {adult, "Give the first dose at once.", "Urinary tract infection in under 16s", true,
         "the title names under 16s"},
    };
    for (const auto& c : cases)
        EXPECT_EQ(PopulationConflict(c.note, c.recommendation, c.title), c.conflict) << c.why;
}

TEST(NearDuplicate, AQualityStatementRestatingItsGuidelineIsADuplicate) {
    const char* const statement =
        "Statement 1 Adults with suspected persistent synovitis affecting more than 1 joint, or "
        "the small joints of the hands and feet, are referred to rheumatology services within 3 "
        "working days of presenting in primary care. [2013, updated 2020]";
    const char* const measure =
        "Adults with pain, swelling and stiffness of more than 1 joint, or the small joints of "
        "the hands or feet, are referred within 3 working days of their GP appointment to a "
        "specialist in rheumatology. Early referral means that they can be diagnosed and start "
        "treatment sooner if they have rheumatoid arthritis.";
    EXPECT_TRUE(NearDuplicate(statement, measure));

    const char* const twelve =
        "12. During bisphosphonate or denosumab therapy, advise patients to report any "
        "unexplained thigh, groin or hip pain and if such symptoms develop, the femur should be "
        "imaged (Strong recommendation).";
    const char* const thirteen =
        "13. If an atypical femoral fracture is identified, image the contralateral femur "
        "(Strong recommendation).";
    EXPECT_FALSE(NearDuplicate(twelve, thirteen)) << "neighbours are not duplicates";
    EXPECT_FALSE(NearDuplicate("Key words: gout.", "Key words: gout, urate."))
        << "too short to judge";
}

}  // namespace
}  // namespace clinicavt::guidance
