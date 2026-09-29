#pragma once

#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "core/guidance/page_text.hpp"

namespace clinicavt::guidance {

// A line is a header or footer if it repeats in the top or bottom band on this share of pages, and
// on at least kFurnitureMinPages
inline constexpr float kFurnitureShare = 0.4F;

inline constexpr int kFurnitureMinPages = 3;

inline constexpr float kFurnitureBand = 0.15F;

// An unmapped glyph that ends this many lines before a lower-case
// continuation is the document's hyphen
inline constexpr int kHyphenLineEnds = 3;

// Unmapped glyphs used as hyphens. Fonts without glyph names return the same code at every
// hyphenated line end
std::set<char32_t> HyphenCodes(const std::vector<Page>& pages);

// Hyphen codes become '-' (en dash between digits). Other unmapped glyphs become U+FFFD next to a
// digit, where they're likely a meaningful symbol, and are dropped otherwise (accents, bullets,
// footnote marks). Expands ligatures, turns no-break spaces into plain ones and squeezes spaces.
// Soft hyphens are dropped except at line end, where JoinBrokenWords needs them
std::string RepairText(std::string_view text, const std::set<char32_t>& hyphens = {});

// Joins words split across lines. Soft hyphens always join. A plain hyphen is removed only if the
// joined word appears elsewhere in the document, which keeps anti-inflammatory
void JoinBrokenWords(Page& page, const std::unordered_set<std::string>& words);

// Removes repeated lines in the top/bottom band. Mid-page repeats (e.g. Recommendation,
// Rationale) are kept
void RemoveFurniture(std::vector<Page>& pages);

// Repairs text, drops empty lines, joins broken words, removes headers and footers
void CleanPages(std::vector<Page>& pages);

// Every word the document uses, lower case, ASCII letters only
std::unordered_set<std::string> DocumentWords(const std::vector<Page>& pages);

}  // namespace clinicavt::guidance
