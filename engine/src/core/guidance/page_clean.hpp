#pragma once

#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "core/guidance/page_text.hpp"

namespace clinicavt::guidance {

// A running header or footer repeats on this share of the pages, at least
// kFurnitureMinPages, inside the top or bottom band of each
inline constexpr float kFurnitureShare = 0.4F;

inline constexpr int kFurnitureMinPages = 3;

inline constexpr float kFurnitureBand = 0.15F;

// An unmapped glyph that ends this many lines before a lower-case
// continuation is the document's hyphen
inline constexpr int kHyphenLineEnds = 3;

// The unmapped glyphs that stand for the document's hyphen: a font without
// names for its glyphs hands the same code back at every hyphenated line end
std::set<char32_t> HyphenCodes(const std::vector<Page>& pages);

// The document's hyphen code becomes a hyphen, or an en dash between digits.
// Any other unmapped glyph stays as U+FFFD only next to a digit, where it was
// a symbol that changed the meaning. Between letters it was an accent and
// alone it was a bullet or a footnote mark, so it goes. Ligatures are spelt
// out, no-break spaces made plain and spaces squeezed. A soft hyphen goes
// unless it ends the line, where the join reads it
std::string RepairText(std::string_view text, const std::set<char32_t>& hyphens = {});

// A word broken at a line end rejoins when the document uses it whole
// elsewhere. A soft hyphen always rejoins. A plain hyphen stays when the
// joined word is unknown, as in anti-inflammatory
void JoinBrokenWords(Page& page, const std::unordered_set<std::string>& words);

// Lines in the top or bottom band whose form repeats across enough pages go.
// A heading that repeats mid-page, like Recommendation or Rationale, stays
void RemoveFurniture(std::vector<Page>& pages);

// The host's pages before anything reads them: text repaired, empty lines
// gone, broken words rejoined, furniture removed
void CleanPages(std::vector<Page>& pages);

// Every word the document uses, lower case, ASCII letters only
std::unordered_set<std::string> DocumentWords(const std::vector<Page>& pages);

}  // namespace clinicavt::guidance
