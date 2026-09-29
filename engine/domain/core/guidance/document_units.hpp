#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/guidance/page_text.hpp"
#include "core/guidance/recommendation_marks.hpp"

namespace clinicavt::guidance {

// Searchable chunk of an added document: a paragraph or run of short ones, with its heading
// and line boxes
struct Unit {
    int page = 0;
    std::string number;   // recommendation number, if any
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

// Most common numbering scheme. A mark counts only if alone or followed by a capital, so
// "R33, R39" in an amendments list isn't counted
Scheme DetectScheme(const std::vector<Paragraph>& paragraphs);

// Groups paragraphs into units. A heading or bare mark labels the next unit. A marked
// paragraph takes its bullets, or runs to its closing token if the document uses them.
// Unmarked paragraphs merge up to kMinUnitWords; ones over kMaxUnitWords are split
std::vector<Unit> UnitsFromParagraphs(const std::vector<Paragraph>& paragraphs);

// Strips a proof's line numbers, then drops the units that are not guidance
std::vector<Unit> DropFragments(std::vector<Unit> units);

// Cleans pages, builds paragraphs, drops the reference tail, then builds units
std::vector<Unit> UnitsFromPages(std::vector<Page>& pages);

std::vector<Unit> UnitsFromText(const std::string& text);

}  // namespace clinicavt::guidance
