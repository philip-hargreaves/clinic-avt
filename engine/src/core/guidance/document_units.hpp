#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/guidance/page_text.hpp"
#include "core/guidance/recommendation_marks.hpp"

namespace clinicavt::guidance {

// What an added document is searched and shown by: a paragraph, or a run of
// short ones, under the heading that preceded it. A box for each line it covers
struct Unit {
    int page = 0;
    std::string number;   // the recommendation's own, when it has one
    std::string section;  // the heading above it
    std::string text;
    std::vector<std::pair<int, Box>> boxes;  // each line's page and box
};

inline constexpr int kHeadingWords = 8;

inline constexpr int kMinUnitWords = 25;

inline constexpr int kMaxUnitWords = 200;

inline constexpr int kSplitWords = 150;

inline constexpr int kTailWords = 6;  // fewer, unnumbered: a sentence's tail or a running head

inline constexpr int kCountRun = 8;  // consecutive integers in a row: a proof's line numbers

inline constexpr int kLegendPairs = 4;  // "ADA: adalimumab; CZP: ..." under a table

inline constexpr int kAffiliations = 3;  // institution words in a block of authors' addresses

inline constexpr int kAuthors = 5;  // names carrying an address number: "Skeoch32,"

// A unit opening with one of these is front matter or a caption
inline constexpr std::string_view kFrontMatter[] = {"key words",
                                                    "keywords",
                                                    "correspondence",
                                                    "received",
                                                    "accepted",
                                                    "conflict of interest",
                                                    "conflicts of interest",
                                                    "funding",
                                                    "disclosure",
                                                    "how to cite",
                                                    "submitted",
                                                    "supplementary data",
                                                    "supplementary material",
                                                    "e-mail",
                                                    "\xC2\xA9",  // the copyright sign
                                                    "this is an open access",
                                                    "all other authors",
                                                    "for permissions",
                                                    "doi",
                                                    "copyright",
                                                    "fig.",
                                                    "fig ",
                                                    "figure ",
                                                    "table "};

// The scheme most paragraphs open with, counting a mark only when it stands
// alone or a capital follows, so "R33, R39" in a list of amendments does not
Scheme DetectScheme(const std::vector<Paragraph>& paragraphs);

// Paragraphs in reading order become units. A heading or a bare mark labels
// the next unit and its lines join that unit's. A marked paragraph opens a
// unit that takes its own bullets, or in a document with closing tokens runs
// to the paragraph ending with one. Unmarked paragraphs merge forward to
// kMinUnitWords, and a paragraph past kMaxUnitWords is split
std::vector<Unit> UnitsFromParagraphs(const std::vector<Paragraph>& paragraphs);

// Strips a proof's line numbers, then drops the units that are not guidance
std::vector<Unit> DropFragments(std::vector<Unit> units);

// From the host's pages to the units stored: cleaned, in paragraphs, the
// reference tail dropped
std::vector<Unit> UnitsFromPages(std::vector<Page>& pages);

std::vector<Unit> UnitsFromText(const std::string& text);

}  // namespace clinicavt::guidance
