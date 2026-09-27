#pragma once

#include <cstddef>
#include <vector>

#include "core/guidance/page_text.hpp"

namespace clinicavt::guidance {

// The reference list: its heading sits in the second half of the paragraphs,
// on its own or as the last line of one, and numbered citations follow. The
// list runs to the next heading or prose paragraph and whatever follows it
// stays, as appendices do in some guidelines. The word alone, as on a
// pathway, drops nothing
inline constexpr int kCitationLines = 6;

inline constexpr int kCitationWindow = 40;

inline constexpr int kListHeadingWords = 8;

inline constexpr int kProseWords = 25;

inline constexpr std::size_t kListLookahead = 3;

void DropReferenceTail(std::vector<Paragraph>& paragraphs);

}  // namespace clinicavt::guidance
