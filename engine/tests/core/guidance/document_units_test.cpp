#include "core/guidance/document_units.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/common/strings.hpp"

namespace clinicavt::guidance {
namespace {

Paragraph Para(const char* text, int page = 0, float top = 0.1F, float bottom = 0.12F) {
    Paragraph p{page, text, {0.1F, top, 0.9F, bottom}, {}};
    p.lines.push_back({text, p.box});
    return p;
}

const char* const kLong =
    "Offer allopurinol after a first attack when urate stays high and the patient agrees.";

TEST(DocumentUnits, HeadingsLabelsAndQuestionsNameTheUnitTheyOpen) {
    // A heading labels the next unit only; a recommendation stands alone
    const auto headed = UnitsFromParagraphs(
        {Para("Gout guideline"), Para("Urate lowering therapy:"),
         Para("1.1 Offer allopurinol after a first attack when urate stays high."),
         Para("1.2 Offer colchicine or an NSAID for an acute flare of gout.")});
    ASSERT_EQ(headed.size(), 2u);
    EXPECT_EQ(headed[0].section, "Urate lowering therapy");
    EXPECT_EQ(headed[0].number, "1.1");
    EXPECT_EQ(headed[0].text, "1.1 Offer allopurinol after a first attack when urate stays high.");
    EXPECT_EQ(headed[1].section, "");
    EXPECT_EQ(headed[1].number, "1.2");

    // The heading's lines join the unit it labels, so the highlight covers both
    const auto prompt = UnitsFromParagraphs(
        {Para("Prompt 1:", 0, 0.30F, 0.32F),
         Para("1.1 Offer allopurinol after a first attack when urate stays high.", 0, 0.34F,
              0.40F)});
    ASSERT_EQ(prompt.size(), 1u);
    EXPECT_EQ(prompt[0].section, "Prompt 1");
    ASSERT_EQ(prompt[0].boxes.size(), 2u);
    EXPECT_FLOAT_EQ(prompt[0].boxes[0].second.top, 0.30F);
    EXPECT_FLOAT_EQ(prompt[0].boxes[1].second.bottom, 0.40F);

    // A bare label numbers the next unit and its lines join too
    const auto labelled = UnitsFromParagraphs(
        {Para("Recommendation 3", 0, 0.30F, 0.32F),
         Para("All people should be assessed for disease (GRADE 1C, SoA 98%).", 0, 0.34F, 0.40F),
         Para("Recommendation 4", 0, 0.42F, 0.44F),
         Para("Treat early (GRADE 2B, SoA 95%).", 0, 0.46F, 0.50F)});
    ASSERT_EQ(labelled.size(), 2u);
    EXPECT_EQ(labelled[0].number, "Recommendation 3");
    EXPECT_EQ(labelled[0].text, "All people should be assessed for disease (GRADE 1C, SoA 98%).");
    ASSERT_EQ(labelled[0].boxes.size(), 2u);
    EXPECT_FLOAT_EQ(labelled[0].boxes[0].second.top, 0.30F);
    EXPECT_EQ(labelled[1].number, "Recommendation 4");

    // A short question is a heading
    const auto asked =
        UnitsFromParagraphs({Para("1.1 Offer allopurinol after a first attack."),
                             Para("1.2 Offer colchicine for an acute flare."),
                             Para("How should suspected GCA be treated?"), Para(kLong)});
    ASSERT_EQ(asked.size(), 3u);
    EXPECT_EQ(asked[2].section, "How should suspected GCA be treated?");
    EXPECT_EQ(asked[2].text, kLong);
}

TEST(DocumentUnits, ShortParagraphsMergeAndLongOnesSplitKeepingEveryLinesBox) {
    const auto merged = UnitsFromParagraphs(
        {Para("Check urate six weeks after any dose change.", 0, 0.10F, 0.12F),
         Para("Titrate the dose in 100 mg steps until urate falls.", 0, 0.13F, 0.15F),
         Para("Stop if a rash appears and refer for advice on alternatives.", 0, 0.16F, 0.18F),
         Para("Review the plan at every appointment with the patient.", 0, 0.19F, 0.21F)});
    ASSERT_EQ(merged.size(), 2u) << "short paragraphs merge forward until the floor";
    EXPECT_EQ(merged[0].text,
              "Check urate six weeks after any dose change. Titrate the dose in 100 mg steps "
              "until urate falls. Stop if a rash appears and refer for advice on alternatives.");
    ASSERT_EQ(merged[0].boxes.size(), 3u);
    EXPECT_FLOAT_EQ(merged[0].boxes[0].second.top, 0.10F);
    EXPECT_FLOAT_EQ(merged[0].boxes[2].second.bottom, 0.18F);
    EXPECT_EQ(merged[1].text, "Review the plan at every appointment with the patient.");

    const auto two_pages = UnitsFromParagraphs(
        {Para("Check urate six weeks after any dose change.", 0, 0.9F, 0.95F),
         Para("Titrate the dose in 100 mg steps until urate falls to target.", 1, 0.05F, 0.1F)});
    ASSERT_EQ(two_pages.size(), 1u);
    EXPECT_EQ(two_pages[0].page, 0);
    ASSERT_EQ(two_pages[0].boxes.size(), 2u);
    EXPECT_EQ(two_pages[0].boxes[1].first, 1) << "each line keeps its own page";

    // A long paragraph splits at sentence ends along its lines
    Paragraph paragraph{0, "", {}, {}};
    for (int i = 0; i < 30; ++i) {
        const float top = 0.1F + i * 0.02F;
        const std::string line = std::string(kLong) + (i % 3 == 2 ? "" : " and");
        if (i) paragraph.text += ' ';
        paragraph.text += line;
        const Box box{0.1F, top, 0.9F, top + 0.015F};
        paragraph.lines.push_back({line, box});
        paragraph.box = i ? Union(paragraph.box, box) : box;
    }
    const auto split = UnitsFromParagraphs({paragraph});
    ASSERT_GE(split.size(), 2u);
    std::size_t lines = 0;
    for (const auto& unit : split) {
        EXPECT_LE(strings::WordCount(unit.text), kMaxUnitWords);
        ASSERT_FALSE(unit.boxes.empty());
        lines += unit.boxes.size();
    }
    EXPECT_EQ(lines, 30u);
    EXPECT_LT(split[0].boxes.back().second.bottom, split[1].boxes.front().second.top + 0.001F);
    EXPECT_TRUE(split[0].text.ends_with("agrees."));
}

TEST(DocumentUnits, ARecommendationRunsToItsClosingTokenAndTakesItsBullets) {
    // A marked recommendation takes its bullets and not the next sentence
    const auto bulleted = UnitsFromParagraphs(
        {Para("1.2.3 Offer allopurinol to people with:"),
         Para("\xE2\x80\xA2 two or more flares a year"), Para("\xE2\x80\xA2 tophi"),
         Para("For a short explanation of why the committee made this recommendation, see the "
              "rationale."),
         Para("1.2.4 Consider febuxostat.")});
    ASSERT_EQ(bulleted.size(), 3u);
    EXPECT_EQ(bulleted[0].number, "1.2.3");
    EXPECT_EQ(bulleted[0].text,
              "1.2.3 Offer allopurinol to people with: \xE2\x80\xA2 two or more flares a year "
              "\xE2\x80\xA2 tophi");
    EXPECT_EQ(bulleted[1].number, "");
    EXPECT_EQ(bulleted[2].number, "1.2.4");

    // A NICE date tag closes it with its bullets and its last sentence
    const auto dated = UnitsFromParagraphs(
        {Para("1.1.5 Refer the person for an assessment if 4 or more criteria are present:"),
         Para("\xE2\x80\xA2 buttock pain"), Para("\xE2\x80\xA2 improvement with movement."),
         Para("If exactly 3 of the criteria are present, perform an HLA-B27 test. [2017]"),
         Para("For a short explanation of why the committee made this recommendation, see the "
              "rationale."),
         Para(
             "1.1.6 Advise the person to seek repeat assessment if new symptoms develop. [2017]")});
    ASSERT_EQ(dated.size(), 3u);
    EXPECT_EQ(dated[0].number, "1.1.5");
    EXPECT_TRUE(dated[0].text.ends_with("perform an HLA-B27 test. [2017]"));
    EXPECT_EQ(dated[1].number, "");
    EXPECT_EQ(dated[2].number, "1.1.6");

    const auto graded = UnitsFromParagraphs(
        {Para("(i) Offer urate lowering therapy after a first flare. LoE: Ia; SOR: 95% (range "
              "80-100%)."),
         Para("(ii) Start allopurinol at 100 mg"), Para("and titrate monthly. LoE: IIb; SOR: 90%."),
         Para("Rationale"),
         Para("Allopurinol is the first-line urate lowering therapy in most patients.")});
    ASSERT_EQ(graded.size(), 3u);
    EXPECT_EQ(graded[0].number, "(i)");
    EXPECT_EQ(graded[1].number, "(ii)");
    EXPECT_EQ(graded[1].text,
              "(ii) Start allopurinol at 100 mg and titrate monthly. LoE: IIb; SOR: 90%.");
    EXPECT_EQ(graded[2].section, "Rationale");

    // A graded table row without marks holds past the length floor until its token
    const auto rows = UnitsFromParagraphs(
        {Para("Offer sulfasalazine when methotrexate is contraindicated or not tolerated in people "
              "with active disease despite other treatment options being considered carefully "
              "first in every single case."),
         Para("Review at three months (GRADE 2C, SoA 92%)."),
         Para("Offer leflunomide as an alternative when sulfasalazine fails or is not tolerated by "
              "the person after an adequate trial at a full dose for three months."),
         Para("Review again (GRADE 2C, SoA 90%).")});
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_TRUE(rows[0].text.ends_with("(GRADE 2C, SoA 92%)."));
    EXPECT_TRUE(rows[1].text.ends_with("(GRADE 2C, SoA 90%)."));
}

TEST(DocumentUnits, FragmentsAreDroppedAndRecommendationsKept) {
    // Each paragraph sits between two recommendations, which set the dotted
    // scheme and keep it a unit of its own. These came from real documents
    struct Case {
        const char* paragraph;
        bool kept;
        const char* text;  // what a kept unit reads, when it differs
    };
    const Case cases[] = {
        {"Fig. 1 Approach to the evaluation of proximal pain and stiffness. ACJ: joint.", false},
        {"Predominant peripheral joint symptoms, X-rays RA, other inflammatory arthritis "
         "Inflammatory Morning stiffness Joint swelling Peripheral hand/foot oedema",
         false},
        {"Key words: Guidelines, Polymyalgia rheumatica, Diagnosis, Treatment.", false},
        {"5.5 Diagnosis", false},
        {"(range 87-100%).", false},
        {"If the patient had rituximab are they enrolled? 59 (81.9) 90 Lipid profile 392 (39.0) "
         "80 Smoking status recorded 613 (61.8) 80 Pregnancy documented 232 (48.3) 80",
         false},
        {"ADA: adalimumab; CZP: certolizumab pegol; ETN: etanercept; GOL: golimumab; IFL: "
         "infliximab; SEC: secukinumab; UST: ustekinumab and the rest of the agents listed.",
         false},
        {"1 Department of Rheumatology, Leeds Teaching Hospitals NHS Trust, Leeds, UK, 2 "
         "Institute of Life Course Sciences, University of Liverpool, Liverpool, UK, and others.",
         false},
        {"Introduction ................................ 2 How to use this Handbook ............ 3 "
         "Context and the national picture you should know ............................... 4",
         false},
        {"Clare Pain12, Georgina Pantano13, John D. Pauling14, Nuala O'Donoghue15, Elisabetta "
         "Renzoni16, Sarah Skeoch32, Dalila Tremarias33 and Chris Wincup34 for the group.",
         false},
        {"Published by Oxford University Press. This is an Open Access article distributed under "
         "the terms of the Creative Commons Attribution License, which permits reuse and "
         "distribution in any medium.",
         false},
        {"Aim for a target serum urate level below 360 micromol/litre.", true},
        {"1.2 Image the femur when thigh pain develops on bisphosphonate therapy", true},
        {"Monitoring: FBC 2 weekly for 6 weeks, then 3 monthly; if WCC < 3.5 you should withhold "
         "and discuss with the rheumatology team on the same day without delay.",
         true},
        // Proof line numbers are stripped
        {"Assess the extent of organ involvement at each visit and target therapy to it, within "
         "the multidisciplinary team, for every person. 8 9 10 11 12 13 14 15 16 17 18",
         true,
         "Assess the extent of organ involvement at each visit and target therapy to it, within "
         "the multidisciplinary team, for every person."},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.paragraph);
        const auto units = DropFragments(UnitsFromParagraphs(
            {Para("1.1 Offer allopurinol after a first attack when urate stays high."),
             Para(c.paragraph), Para("1.9 Offer colchicine or an NSAID for an acute flare.")}));
        ASSERT_EQ(units.size(), c.kept ? 3u : 2u);
        EXPECT_EQ(units.front().number, "1.1");
        EXPECT_EQ(units.back().number, "1.9");
        if (c.kept) EXPECT_EQ(units[1].text, c.text != nullptr ? c.text : c.paragraph);
    }
}

TEST(DocumentUnits, PlainTextBecomesUnitsWithoutBoxes) {
    // The .md and .txt path: blank lines split paragraphs, a paragraph's lines join
    const auto units = UnitsFromText(
        "Gout  guideline\n\n1.1 Offer allopurinol after a first attack\nwhen urate stays high.\n"
        "\n\n1.2 Offer colchicine or an NSAID for an acute flare.\n\n"
        "Key words: gout, urate lowering therapy.\n");
    ASSERT_EQ(units.size(), 2u) << "the key words line is front matter";
    EXPECT_EQ(units[0].section, "Gout guideline");
    EXPECT_EQ(units[0].number, "1.1");
    EXPECT_EQ(units[0].text, "1.1 Offer allopurinol after a first attack when urate stays high.");
    EXPECT_EQ(units[0].page, 0);
    EXPECT_EQ(units[1].number, "1.2");
    for (const auto& unit : units) EXPECT_TRUE(unit.boxes.empty()) << unit.text;
}

PageLine At(const std::string& text, float top, float right = 0.9F) {
    return {text, {0.1F, top, right, top + 0.012F}};
}

TEST(DocumentUnits, APdfsPagesLoseFurnitureReferencesAndFragmentsOnTheWayToUnits) {
    std::vector<Page> pages(3);
    pages[0].lines = {
        At("Recommendations", 0.20F, 0.35F),
        At("1.1 Offer allopurinol to people with gout after a first flare when their serum urate",
           0.22F),
        At("stays high, and titrate the dose while treat-", 0.24F),
        At("ment continues until urate is below target.", 0.26F, 0.6F),
        At("1.2 Offer colchicine or a non-steroidal anti-inflammatory drug as \xEF\xAC\x81rst-line",
           0.30F),
        At("treatment for a gout flare, taking account of the person's preferences.", 0.32F, 0.7F)};
    pages[1].lines = {
        At("Figure 1 Treatment pathway for a gout flare in primary care, from first contact to "
           "review.",
           0.20F),
        At("1.3 Consider febuxostat for people who cannot take allopurinol or in whom it has not",
           0.30F),
        At("lowered urate to target.", 0.32F, 0.7F)};
    pages[2].lines = {
        At("1.4 Measure serum urate two to four weeks after a flare has settled, and repeat",
           0.20F),
        At("it every six months once the target is reached.", 0.22F, 0.6F),
        At("1.5 Refer people with gout to a rheumatology service when the diagnosis is uncertain.",
           0.26F),
        At("References", 0.32F, 0.3F)};
    for (int i = 0; i < 6; ++i) {
        pages[2].lines.push_back(At(std::to_string(i + 1) +
                                        " Kuo CF, Grainge MJ. Rising burden of gout in the UK. "
                                        "Ann Rheum Dis 2015;74:661-7.",
                                    0.34F + 0.02F * static_cast<float>(i)));
    }
    for (std::size_t p = 0; p < pages.size(); ++p) {
        auto& lines = pages[p].lines;
        lines.insert(lines.begin(), At("Gout: diagnosis and management", 0.03F));
        lines.push_back(At("Page " + std::to_string(p + 1) + " of 3", 0.96F));
    }

    const auto units = UnitsFromPages(pages);

    ASSERT_EQ(units.size(), 5u);
    const char* const numbers[] = {"1.1", "1.2", "1.3", "1.4", "1.5"};
    for (std::size_t i = 0; i < units.size(); ++i) {
        EXPECT_EQ(units[i].number, numbers[i]) << units[i].text;
        for (const char* gone : {"Gout: diagnosis", "Page ", "Kuo CF", "Figure 1"})
            EXPECT_EQ(units[i].text.find(gone), std::string::npos) << units[i].text;
    }
    EXPECT_EQ(units[0].section, "Recommendations");
    EXPECT_EQ(units[0].text,
              "1.1 Offer allopurinol to people with gout after a first flare when their serum "
              "urate stays high, and titrate the dose while treatment continues until urate is "
              "below target.")
        << "the broken word rejoins because the document uses it whole";
    EXPECT_EQ(units[0].boxes.size(), 4u) << "the heading's line and the three it labels";
    EXPECT_EQ(units[1].text,
              "1.2 Offer colchicine or a non-steroidal anti-inflammatory drug as first-line "
              "treatment for a gout flare, taking account of the person's preferences.");
    EXPECT_EQ(units[2].page, 1);
    EXPECT_EQ(units[3].page, 2);
    EXPECT_EQ(units[4].page, 2);
}

}  // namespace
}  // namespace clinicavt::guidance
