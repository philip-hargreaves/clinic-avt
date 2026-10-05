#include "core/guidance/reference_tail.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace clinicavt::guidance {
namespace {

Paragraph Para(const std::string& text) {
    Paragraph p{0, text, {}, {}};
    p.lines.push_back({text, {}});
    return p;
}

std::vector<Paragraph> Body(int count) {
    std::vector<Paragraph> out;
    for (int i = 0; i < count; ++i) out.push_back(Para("Offer allopurinol after a first attack."));
    return out;
}

std::vector<Paragraph> Citations(int count) {
    std::vector<Paragraph> out;
    for (int i = 0; i < count; ++i) {
        out.push_back(
            Para(std::to_string(i + 1) + " Kuo CF, Grainge MJ. Gout. Lancet 2015;74:661."));
    }
    return out;
}

TEST(ReferenceTail, TheTailGoesFromItsHeadingButAnAppendixAfterItStays) {
    auto paragraphs = Body(30);
    paragraphs.push_back(Para("8 References"));
    const auto citations = Citations(8);
    paragraphs.insert(paragraphs.end(), citations.begin(), citations.end());
    paragraphs.push_back(Para("2010;62:1060-8."));
    paragraphs.push_back(Para("Nat Rev Rheumatol"));
    paragraphs.push_back(
        Para("46. de Man YA, Hazes JM, van der Heide H et al. Association of higher "
             "rheumatoid arthritis disease activity during pregnancy with lower "
             "birth weight and preterm delivery, results of a national prospective "
             "study across many centres with"));
    paragraphs.push_back(
        Para("lower birth weight: results of a national prospective study. Arthritis "
             "Rheum 2009;60:3196-206."));
    paragraphs.push_back(
        Para("9 Roddy E, Zhang W, Doherty M. Evidence-based guidelines. J Eval Clin "
             "Pract 2006;12:347-52."));
    paragraphs.push_back(Para("Supplementary Table 1 Doses in pregnancy"));
    paragraphs.push_back(
        Para("1 Hydroxychloroquine 400 mg daily is compatible (GRADE 1B, SoA 100%)."));
    DropReferenceTail(paragraphs);
    ASSERT_EQ(paragraphs.size(), 32u);
    EXPECT_EQ(paragraphs[30].text, "Supplementary Table 1 Doses in pregnancy");

    // A heading that ends a paragraph takes only the tail
    auto joined = Body(30);
    Paragraph last = Para("Review the plan at every appointment.");
    last.lines.push_back({"References", {}});
    last.text += " References";
    joined.push_back(last);
    joined.insert(joined.end(), citations.begin(), citations.end());
    DropReferenceTail(joined);
    ASSERT_EQ(joined.size(), 31u);
    EXPECT_EQ(joined.back().text, "Review the plan at every appointment.");
    EXPECT_EQ(joined.back().lines.size(), 1u);
}

TEST(ReferenceTail, TheWordAloneOrEarlyDropsNothing) {
    auto pathway = Body(2);
    pathway.push_back(Para("References"));
    pathway.push_back(Para("See the local formulary for doses."));
    DropReferenceTail(pathway);
    EXPECT_EQ(pathway.size(), 4u);

    auto early = Citations(8);
    early.insert(early.begin(), Para("References"));
    const auto body = Body(20);
    early.insert(early.end(), body.begin(), body.end());
    DropReferenceTail(early);
    EXPECT_EQ(early.size(), 29u);
}

}  // namespace
}  // namespace clinicavt::guidance
