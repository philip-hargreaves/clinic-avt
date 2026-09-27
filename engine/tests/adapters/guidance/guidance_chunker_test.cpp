#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "tools/corpus/chunker.hpp"

namespace clinicavt::guidance {
namespace {

nlohmann::json Document() {
    return nlohmann::json::parse(R"json({
        "code": "fx100",
        "title": "Fictional inflammatory joint disease",
        "source_url": "https://example.test/guidance/fx100",
        "last_updated": "2026-01-01",
        "chapters": [
            {"title": "Recommendations", "slug": "Recommendations", "recommendations": [
                {"id": "fx100-1_1_1", "kind": "recommendation", "number": "1.1.1",
                 "section": "1.1 Referral", "update_tag": "2026", "text": "  Refer adults with persistent synovitis.  "},
                {"id": "fx100-1_1_2", "kind": "recommendation", "number": "1.1.2",
                 "section": "1.1 Referral", "text": "Refer urgently if the small joints are affected."},
                {"id": "fx100-p_3", "kind": "numbered_paragraph", "number": "3", "text": "Not a recommendation."}
            ]},
            {"title": "Recommendations (copy)", "slug": "recommendations", "recommendations": [
                {"id": "fx100-1_1_1", "kind": "recommendation", "number": "1.1.1",
                 "section": "1.1 Referral", "text": "Refer adults with persistent synovitis."},
                {"id": "fx100-1_2_1", "kind": "recommendation", "number": "1.2.1",
                 "section": "1.2 Investigations", "text": "Offer a blood test for rheumatoid factor."}
            ]}
        ]
})json");
}

TEST(ChunksFromDocument, OneChunkPerRecommendationWithDuplicatesAndParagraphsSkipped) {
    std::set<std::string> seen;
    const auto chunks = ChunksFromDocument(Document(), seen);
    ASSERT_EQ(chunks.size(), 3u);
    EXPECT_EQ(chunks[0].id, "fx100-1_1_1");
    EXPECT_EQ(chunks[0].text, "Refer adults with persistent synovitis.") << "trimmed";
    EXPECT_EQ(chunks[0].section, "1.1 Referral");
    EXPECT_EQ(chunks[0].number, "1.1.1");
    EXPECT_EQ(chunks[0].update_tag, "2026");
    EXPECT_EQ(chunks[0].last_updated, "2026-01-01");
    EXPECT_EQ(chunks[0].url,
              "https://example.test/guidance/fx100/chapter/Recommendations#fx100-1_1_1");
    EXPECT_EQ(chunks[1].id, "fx100-1_1_2");
    EXPECT_EQ(chunks[2].id, "fx100-1_2_1") << "the copied chapter contributes only its new id";
    EXPECT_EQ(chunks[2].chapter, "Recommendations (copy)");
    EXPECT_EQ(seen.size(), 3u);
}

std::string Paragraph(int words) {
    std::string out;
    for (int i = 0; i < words; ++i) out += (i ? " word" : "word");
    return out;
}

TEST(ChunksFromText, RunsCloseAtHeadingsAndTheTargetLengthAndNeverExceedTheMaximum) {
    const std::string text = "A short title\n\n" + Paragraph(20) + "\n\n" + Paragraph(20) +
                             "\n\n1.1.1 " + Paragraph(30) + "\n \n" + Paragraph(280) + "\n\n" +
                             Paragraph(40) + "\n\nRecommendation 2 " + Paragraph(10);
    const auto chunks = ChunksFromText("doc", "A document", text, "doc.md");
    ASSERT_EQ(chunks.size(), 4u) << chunks.size();
    EXPECT_EQ(chunks[0].id, "doc-1");
    EXPECT_EQ(chunks[0].text, Paragraph(20) + " " + Paragraph(20)) << "the 3-word title is dropped";
    EXPECT_EQ(chunks[0].number, "");
    EXPECT_EQ(chunks[1].number, "1.1.1") << "a heading opens a run and names it";
    EXPECT_TRUE(chunks[1].section.empty()) << "plain text carries no section";
    EXPECT_EQ(chunks[1].text.substr(0, 6), "1.1.1 ");
    EXPECT_EQ(chunks[2].text, Paragraph(40)) << "the run before it reached the target length";
    EXPECT_EQ(chunks[3].text.substr(0, 16), "Recommendation 2");
    EXPECT_EQ(chunks[3].url, "doc.md");
    EXPECT_TRUE(ChunksFromText("x", "x", "too short\n\nalso short").empty());

    std::string long_text;
    for (int i = 0; i < 10; ++i) long_text += Paragraph(250) + "\n\n";
    for (const auto& chunk : ChunksFromText("doc", "A document", long_text)) {
        int words = 1;
        for (char c : chunk.text) words += c == ' ';
        EXPECT_LE(words, kMaxWords);
    }

    // Only a numbered opening is a heading, never a dose or a version
    const std::pair<const char*, bool> openings[] = {
        {"Recommendation 12", true},    {"recommendation 3a Consider a blood test", true},
        {"1.2 Investigations", true},   {"1.2.3 Offer first-line treatment", true},
        {"1 Introduction", false},      {"Version 1.2 of the guideline", false},
        {"10 mg twice a day", false},   {"Recommendations for research", false},
        {"1.2.3mg is the dose", false},
    };
    for (const auto& [opening, starts] : openings)
        EXPECT_EQ(StartsRecommendation(opening), starts) << opening;
}

}  // namespace
}  // namespace clinicavt::guidance
