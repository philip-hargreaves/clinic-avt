#pragma once

#include <cstddef>
#include <vector>

#include "core/guidance/page_text.hpp"

namespace clinicavt::guidance {

// Drops a References heading (alone or a paragraph's last line) in the second half that is followed
// by numbered citations, with its list up to the next heading or prose. Later content such as
// appendices is kept. A heading with no citations drops nothing
inline constexpr int kCitationLines = 6;

inline constexpr int kCitationWindow = 40;

inline constexpr int kListHeadingWords = 8;

inline constexpr int kProseWords = 25;

inline constexpr std::size_t kListLookahead = 3;

void DropReferenceTail(std::vector<Paragraph>& paragraphs);

}  // namespace clinicavt::guidance
