#include "core/guidance/page_text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include "core/common/strings.hpp"
#include "core/guidance/guidance_query.hpp"
#include "core/guidance/recommendation_marks.hpp"

namespace clinicavt::guidance {

namespace {

// The page's leading: the median rise from one line's top to the next
float Pitch(const Page& page) {
    std::vector<float> rises;
    for (std::size_t k = 0; k + 1 < page.lines.size(); ++k) {
        const float rise = page.lines[k + 1].box.top - page.lines[k].box.top;
        if (rise > 0) rises.push_back(rise);
    }
    if (rises.empty()) return 0;
    const auto middle = rises.begin() + static_cast<std::ptrdiff_t>(rises.size() / 2);
    std::nth_element(rises.begin(), middle, rises.end());
    return *middle;
}

// Where lines end on each half of the page: the median right edge
std::array<float, 2> ColumnEdges(const Page& page) {
    std::array<std::vector<float>, 2> rights;
    for (const auto& line : page.lines) {
        rights[line.box.left > 0.5F].push_back(line.box.right);
    }
    std::array<float, 2> edges{1, 1};
    for (std::size_t side = 0; side < 2; ++side) {
        auto& r = rights[side];
        if (r.empty()) continue;
        const auto middle = r.begin() + static_cast<std::ptrdiff_t>(r.size() / 2);
        std::nth_element(r.begin(), middle, r.end());
        edges[side] = *middle;
    }
    return edges;
}

bool Short(const PageLine& line, const std::array<float, 2>& edges) {
    return line.box.right < edges[line.box.left > 0.5F] - kShortLine;
}

// Line opens with a mark and a capital, e.g. "1.5.7 Consider". NICE sets list items at normal
// line spacing, so leading alone can't split them
bool OpensMarked(const std::string& text) {
    for (const auto scheme : kMarkSchemes) {
        const auto mark = MarkOf(text, scheme);
        if (!mark.empty() && mark.size() + 1 < text.size() && text[mark.size()] == ' ' &&
            std::isupper(static_cast<unsigned char>(text[mark.size() + 1]))) {
            return true;
        }
    }
    return false;
}

}  // namespace

Box Union(const Box& a, const Box& b) {
    return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right),
            std::max(a.bottom, b.bottom)};
}

std::vector<Paragraph> ParagraphsFromPages(const std::vector<Page>& pages) {
    std::vector<Paragraph> out;
    int last_page = -1;
    float last_top = 0;
    bool last_short = false;
    for (std::size_t p = 0; p < pages.size(); ++p) {
        const float pitch = Pitch(pages[p]);
        const auto edges = ColumnEdges(pages[p]);
        for (const auto& line : pages[p].lines) {
            if (line.text.empty()) continue;
            const float height = line.box.bottom - line.box.top;
            const float rise = line.box.top - last_top;
            const float limit = pitch > 0 ? kAdjacentPitch * pitch : 1.5F * height;
            const auto first = static_cast<unsigned char>(line.text.front());
            const bool adjacent = !out.empty() && static_cast<int>(p) == last_page &&
                                  rise > -0.5F * height && rise <= limit &&
                                  !(last_short && OpensSentence(first)) &&
                                  !OpensMarked(line.text) && !EndsWithToken(out.back().text);
            const bool runs_on =
                !out.empty() && !strings::EndsSentence(out.back().text) && std::islower(first);
            // A wrapped grade tail always rejoins the recommendation above it
            const bool tail = !out.empty() && IsGradeTail(line.text);
            if (adjacent || runs_on || tail) {
                auto& para = out.back();
                para.text += ' ';
                para.text += line.text;
                para.box = Union(para.box, line.box);
                para.lines.push_back(line);
            } else {
                out.push_back({static_cast<int>(p), line.text, line.box, {line}});
            }
            last_page = static_cast<int>(p);
            last_top = line.box.top;
            last_short = Short(line, edges);
        }
    }
    return out;
}

std::vector<Paragraph> ParagraphsFromText(const std::string& text) {
    std::vector<Paragraph> out;
    Paragraph current;
    const auto flush = [&] {
        if (!current.lines.empty()) out.push_back(std::move(current));
        current = Paragraph{};
    };
    std::size_t at = 0;
    while (at <= text.size()) {
        const auto end = text.find('\n', at);
        const auto raw = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const std::string line = strings::Squeeze(raw);
        if (line.empty()) {
            flush();
        } else {
            if (!current.lines.empty()) current.text += ' ';
            current.text += line;
            current.lines.push_back({line, {}});
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    flush();
    return out;
}

}  // namespace clinicavt::guidance
