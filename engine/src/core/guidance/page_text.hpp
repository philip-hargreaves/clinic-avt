#pragma once

#include <string>
#include <vector>

#include "core/guidance/recommendation_marks.hpp"

namespace clinicavt::guidance {

// A region of a page as fractions of its displayed size, origin top left
struct Box {
    float left = 0;
    float top = 0;
    float right = 0;
    float bottom = 0;
};

struct PageLine {
    std::string text;
    Box box;
};

// One page as the ingest host read it
struct Page {
    float width = 0;  // points, as displayed
    float height = 0;
    std::vector<PageLine> lines;
};

// A run of lines on one page, joined with spaces, the lines kept for splitting
struct Paragraph {
    int page = 0;
    std::string text;
    Box box;
    std::vector<PageLine> lines;
};

Box Union(const Box& a, const Box& b);

// A line follows the one before within this many of the page's leading
inline constexpr float kAdjacentPitch = 1.3F;

// A line ending this far short of its column's edge closes a paragraph when a
// capital follows
inline constexpr float kShortLine = 0.1F;

// Lines in the host's order, which is the reading order of the PDFs measured.
// A paragraph closes when the next line rises more than the page's leading
// allows, jumps back up the page, opens with a capital after a short line,
// opens a marked recommendation, or follows a closing token. It runs on
// regardless when it stopped mid-sentence and the next line begins in lower
// case, across a column or a page
std::vector<Paragraph> ParagraphsFromPages(const std::vector<Page>& pages);

// Plain or Markdown text: paragraphs split on blank lines, each line kept,
// whitespace runs collapsed to one space, boxes empty
std::vector<Paragraph> ParagraphsFromText(const std::string& text);

}  // namespace clinicavt::guidance
