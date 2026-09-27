#include "core/guidance/reference_tail.hpp"

#include <cctype>
#include <cstddef>
#include <string_view>
#include <vector>

#include "core/common/strings.hpp"
#include "core/guidance/page_text.hpp"

namespace clinicavt::guidance {

namespace {

// "References", "Reference list" or "Bibliography", with or without a section number
bool IsReferencesHeading(std::string_view text) {
    auto s = strings::Trim(text);
    std::size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
    const auto lower = strings::Lower(strings::Trim(s.substr(i)));
    return lower == "references" || lower == "reference list" || lower == "bibliography";
}

// "12 Kuo CF", "[12] Kuo" or "12. Kuo": a numbered entry
bool IsCitationLine(std::string_view text) {
    std::size_t i = text.starts_with('[') ? 1 : 0;
    const auto start = i;
    while (i < text.size() && i - start < 3 && std::isdigit(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    if (i == start) return false;
    if (i < text.size() && (text[i] == ']' || text[i] == '.')) ++i;
    return i < text.size() && text[i] == ' ';
}

bool CitationsFollow(const std::vector<Paragraph>& paragraphs, std::size_t heading) {
    int seen = 0;
    int citations = 0;
    for (std::size_t j = heading + 1; j < paragraphs.size() && seen < kCitationWindow; ++j) {
        for (const auto& line : paragraphs[j].lines) {
            if (seen++ >= kCitationWindow) break;
            citations += IsCitationLine(line.text);
        }
    }
    return citations >= kCitationLines;
}

bool HasYear(std::string_view text) {
    for (std::size_t i = 0; i + 4 <= text.size(); ++i) {
        if ((text.substr(i, 2) == "19" || text.substr(i, 2) == "20") &&
            std::isdigit(static_cast<unsigned char>(text[i + 2])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 3])) &&
            (i == 0 || !std::isdigit(static_cast<unsigned char>(text[i - 1]))) &&
            (i + 4 == text.size() || !std::isdigit(static_cast<unsigned char>(text[i + 4])))) {
            return true;
        }
    }
    return false;
}

// A numbered entry that names a year
bool Cites(const Paragraph& paragraph) {
    return IsCitationLine(paragraph.lines.front().text) && HasYear(paragraph.text);
}

// The list ends at prose without a year, or at a heading that no citation
// follows within kListLookahead paragraphs. A numbered entry carries it on
// even when a page break has parted it from its year, as do the fragments of
// a split entry, a journal and year on their own line
bool EndsList(const std::vector<Paragraph>& paragraphs, std::size_t i) {
    const auto& text = paragraphs[i].text;
    if (text.empty() || IsCitationLine(paragraphs[i].lines.front().text)) return false;
    const auto words = strings::WordCount(text);
    if (words >= kProseWords) return !HasYear(text);
    const bool heading = words <= kListHeadingWords && !strings::EndsSentence(text) &&
                         std::isupper(static_cast<unsigned char>(text[0])) && !HasYear(text);
    if (!heading) return false;
    for (std::size_t j = i + 1; j < paragraphs.size() && j <= i + kListLookahead; ++j) {
        if (Cites(paragraphs[j])) return false;
    }
    return true;
}

void DropLastLine(Paragraph& paragraph) {
    paragraph.lines.pop_back();
    paragraph.text.clear();
    for (const auto& line : paragraph.lines) {
        if (!paragraph.text.empty()) paragraph.text += ' ';
        paragraph.text += line.text;
    }
}

}  // namespace

void DropReferenceTail(std::vector<Paragraph>& paragraphs) {
    for (std::size_t i = paragraphs.size() / 2; i < paragraphs.size(); ++i) {
        auto& paragraph = paragraphs[i];
        const bool whole = IsReferencesHeading(paragraph.text);
        const bool last_line = !whole && paragraph.lines.size() > 1 &&
                               IsReferencesHeading(paragraph.lines.back().text);
        if ((!whole && !last_line) || !CitationsFollow(paragraphs, i)) continue;
        auto end = i + 1;
        while (end < paragraphs.size() && !EndsList(paragraphs, end)) ++end;
        if (last_line) DropLastLine(paragraph);
        paragraphs.erase(paragraphs.begin() + static_cast<std::ptrdiff_t>(i + (last_line ? 1 : 0)),
                         paragraphs.begin() + static_cast<std::ptrdiff_t>(end));
        return;
    }
}

}  // namespace clinicavt::guidance
