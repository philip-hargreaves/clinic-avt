#include "core/guidance/document_units.hpp"

#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/common/strings.hpp"
#include "core/guidance/page_clean.hpp"
#include "core/guidance/page_text.hpp"
#include "core/guidance/recommendation_marks.hpp"
#include "core/guidance/reference_tail.hpp"

namespace clinicavt::guidance {

namespace {

std::string WithoutStop(std::string_view s) {
    while (!s.empty() && (s.back() == '.' || s.back() == ':' || s.back() == ' ')) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

// A bullet or lower-case start continues the previous recommendation's list
bool Continues(std::string_view text) {
    return text.starts_with("\xE2\x80\xA2") || text.starts_with("\xE2\x80\x93") ||
           text.starts_with('-') || text.starts_with('*') ||
           (!text.empty() && std::islower(static_cast<unsigned char>(text.front())));
}

// Short, unmarked, no sentence end or bullet (titles, captions, labels). Short questions count
bool IsHeading(const Paragraph& paragraph, Scheme scheme) {
    const std::string_view text = paragraph.text;
    return strings::WordCount(text) < kHeadingWords &&
           (!strings::EndsSentence(text) || text.ends_with('?')) && MarkOf(text, scheme).empty() &&
           !Continues(text);
}

// The token as a whole number, or -1
long Integer(std::string_view token) {
    if (token.empty() || token.size() > 6) return -1;
    long value = 0;
    for (const char c : token) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return -1;
        value = value * 10 + (c - '0');
    }
    return value;
}

// Drops runs of consecutive integers (proof-copy line numbers)
std::string WithoutCounts(std::string_view text) {
    const auto tokens = strings::Words(text);
    std::string out;
    for (std::size_t i = 0; i < tokens.size();) {
        std::size_t end = i + 1;
        while (end < tokens.size() && Integer(tokens[end - 1]) >= 0 &&
               Integer(tokens[end]) == Integer(tokens[end - 1]) + 1) {
            ++end;
        }
        if (end - i >= static_cast<std::size_t>(kCountRun)) {
            i = end;
            continue;
        }
        if (!out.empty()) out += ' ';
        out += tokens[i++];
    }
    return out;
}

// Figures, ranges and citation numbers: "59", "(81.9)", "[26-29,", "40.5%"
bool Numeric(std::string_view token) {
    bool digit = false;
    for (const unsigned char c : token) {
        if (std::isdigit(c)) {
            digit = true;
        } else if (std::isalpha(c)) {
            return false;
        }
    }
    return digit;
}

int CountWords(std::string_view lower, std::initializer_list<std::string_view> among,
               int* total = nullptr) {
    int found = 0;
    for (const auto& word :
         strings::LowerTokens(lower, [](unsigned char c) { return std::isalpha(c) != 0; })) {
        if (total != nullptr) ++*total;
        for (const auto one : among) found += word == one;
    }
    return found;
}

// Directive wording; such units are kept even if they look like tables or addresses
bool Guides(std::string_view lower) {
    return CountWords(lower, {"should", "recommend", "recommended", "offer", "consider", "refer"}) >
           0;
}

// Unnumbered and 40%+ numeric, or an abbreviation legend
bool IsTable(const Unit& unit) {
    const auto tokens = strings::Words(unit.text);
    if (tokens.empty()) return false;
    int numeric = 0;
    for (const auto token : tokens) numeric += Numeric(token);
    if (unit.number.empty() && numeric * 10 >= static_cast<int>(tokens.size()) * 4) return true;
    int pairs = 0;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        const bool opens = i == 0 || tokens[i - 1].ends_with(';');
        pairs += opens && tokens[i].ends_with(':') && tokens[i].size() <= 13;
    }
    return pairs >= kLegendPairs && tokens[0].ends_with(':');
}

// Names with affiliation numbers, e.g. "Skeoch32,"
bool IsAuthors(std::string_view text) {
    int names = 0;
    for (const auto token : strings::Words(text)) {
        auto end = token.size();
        while (end > 0 && (token[end - 1] == ',' ||
                           std::isdigit(static_cast<unsigned char>(token[end - 1])))) {
            --end;
        }
        const bool numbered = token.find_first_of("0123456789", end) != std::string_view::npos;
        names += numbered && end > 2 && std::isalpha(static_cast<unsigned char>(token[end - 1]));
    }
    return names >= kAuthors;
}

// Affiliation block, dense in institution words
bool IsAffiliations(std::string_view lower) {
    int words = 0;
    const int institutions = CountWords(
        lower,
        {"university", "hospital", "hospitals", "department", "institute", "centre", "center",
         "nhs", "trust", "school", "college", "foundation", "division", "faculty"},
        &words);
    return institutions >= kAffiliations && institutions * 14 >= words;
}

// Splits on line boundaries: at a sentence end after kSplitWords, or at kMaxUnitWords
std::vector<Paragraph> Split(const Paragraph& paragraph) {
    std::vector<Paragraph> out;
    Paragraph piece{paragraph.page, "", {}, {}};
    int words = 0;
    const auto close = [&] {
        if (piece.lines.empty()) return;
        out.push_back(std::move(piece));
        piece = Paragraph{paragraph.page, "", {}, {}};
        words = 0;
    };
    for (const auto& line : paragraph.lines) {
        if (!piece.lines.empty()) piece.text += ' ';
        piece.text += line.text;
        piece.box = piece.lines.empty() ? line.box : Union(piece.box, line.box);
        piece.lines.push_back(line);
        words += strings::WordCount(line.text);
        if ((words >= kSplitWords && strings::EndsSentence(line.text)) || words >= kMaxUnitWords)
            close();
    }
    close();
    return out;
}

// Whether a token closes the unit within the paragraphs ahead, before a
// heading, a mark or kMaxUnitWords, so a graded table row stays whole
bool TokenAhead(const std::vector<Paragraph>& paragraphs, std::size_t from, int words,
                Scheme scheme) {
    for (std::size_t i = from; i < paragraphs.size(); ++i) {
        const auto& paragraph = paragraphs[i];
        if (i > from && (IsHeading(paragraph, scheme) || !MarkOf(paragraph.text, scheme).empty())) {
            return false;
        }
        words += strings::WordCount(paragraph.text);
        if (words > kMaxUnitWords) return false;
        if (EndsWithToken(paragraph.text)) return true;
    }
    return false;
}

}  // namespace

Scheme DetectScheme(const std::vector<Paragraph>& paragraphs) {
    Scheme best = Scheme::kNone;
    int best_count = kSchemeMarks - 1;
    for (const auto scheme : kMarkSchemes) {
        int count = 0;
        for (const auto& paragraph : paragraphs) {
            const auto mark = MarkOf(paragraph.text, scheme);
            if (mark.empty()) continue;
            const auto rest = strings::Trim(std::string_view(paragraph.text).substr(mark.size()));
            if (rest.empty() || std::isupper(static_cast<unsigned char>(rest[0])) ||
                rest[0] == '"') {
                ++count;
            }
        }
        if (count > best_count) {
            best = scheme;
            best_count = count;
        }
    }
    return best;
}

std::vector<Unit> UnitsFromParagraphs(const std::vector<Paragraph>& paragraphs) {
    const auto scheme = DetectScheme(paragraphs);
    int closed = 0;
    for (const auto& paragraph : paragraphs) closed += EndsWithToken(paragraph.text);
    const bool tokens = closed >= kTokenParagraphs;

    std::vector<Unit> out;
    Unit current;
    std::string heading;
    std::string number;
    const Paragraph* label = nullptr;
    int words = 0;
    bool open = false;
    bool marked = false;
    const auto close = [&] {
        if (open) out.push_back(std::move(current));
        current = Unit{};
        words = 0;
        open = false;
        marked = false;
    };
    const auto mark_lines = [&](const Paragraph& paragraph) {
        for (const auto& line : paragraph.lines) {
            if (line.box.right > line.box.left) {
                current.boxes.emplace_back(paragraph.page, line.box);
            }
        }
    };
    const auto take = [&](const Paragraph& paragraph, std::string_view mark) {
        if (!open) {
            current.page = paragraph.page;
            current.section = heading;
            current.number = number.empty() ? WithoutStop(mark) : number;
            marked = !current.number.empty();
            if (label != nullptr) mark_lines(*label);
            heading.clear();
            number.clear();
            label = nullptr;
            open = true;
        } else {
            current.text += ' ';
        }
        current.text += paragraph.text;
        mark_lines(paragraph);
        words += strings::WordCount(paragraph.text);
    };
    for (std::size_t i = 0; i < paragraphs.size(); ++i) {
        const auto& paragraph = paragraphs[i];
        auto mark = MarkOf(paragraph.text, scheme);
        if (IsHeading(paragraph, scheme)) {
            close();
            label = &paragraph;
            heading = WithoutStop(paragraph.text);
            continue;
        }
        if (!mark.empty() && mark.size() == paragraph.text.size()) {
            close();
            label = &paragraph;
            number = WithoutStop(mark);
            continue;
        }
        // NOLINTNEXTLINE(bugprone-branch-clone) order matters, so the matching branches stay apart
        if (!mark.empty()) {
            close();
        } else if (marked && !tokens) {
            if (!Continues(paragraph.text)) close();
        } else if (words >= kMinUnitWords &&
                   !(tokens && TokenAhead(paragraphs, i, words, scheme))) {
            close();
        }
        if (strings::WordCount(paragraph.text) > kMaxUnitWords) {
            close();
            for (const auto& piece : Split(paragraph)) {
                take(piece, mark);
                mark = {};
                close();
            }
            continue;
        }
        take(paragraph, mark);
        if (tokens && EndsWithToken(paragraph.text)) close();
    }
    close();
    return out;
}

namespace {

// Not guidance: short unmarked text (labels, table fragments, sentence tails), numbered
// headings, contents pages, front matter, captions, and tables or addresses without
// directive wording
bool IsFragment(const Unit& unit) {
    const int words = strings::WordCount(unit.text);
    if (unit.number.empty() &&
        (words < kTailWords || (words < kMinUnitWords && !strings::EndsSentence(unit.text)))) {
        return true;
    }
    // "5.5 Diagnosis": a numbered heading the scheme took for a recommendation
    if (!unit.number.empty() && words < kHeadingWords && !strings::EndsSentence(unit.text) &&
        !unit.text.ends_with(':')) {
        return true;
    }
    if (strings::Contains(unit.text, ".....")) return true;  // a contents page's dot leaders
    const auto lower = strings::Lower(unit.text);
    if (!Guides(lower) && (IsTable(unit) || IsAffiliations(lower) || IsAuthors(unit.text) ||
                           strings::Contains(lower, "creative commons"))) {
        return true;
    }
    const auto head = strings::Lower(std::string_view(unit.text).substr(0, 24));
    for (const auto label : kFrontMatter) {
        if (head.starts_with(label)) return true;
    }
    return false;
}

}  // namespace

std::vector<Unit> DropFragments(std::vector<Unit> units) {
    for (auto& unit : units) unit.text = WithoutCounts(unit.text);
    std::erase_if(units, IsFragment);
    return units;
}

std::vector<Unit> UnitsFromPages(std::vector<Page>& pages) {
    CleanPages(pages);
    auto paragraphs = ParagraphsFromPages(pages);
    DropReferenceTail(paragraphs);
    return DropFragments(UnitsFromParagraphs(paragraphs));
}

std::vector<Unit> UnitsFromText(const std::string& text) {
    auto paragraphs = ParagraphsFromText(text);
    DropReferenceTail(paragraphs);
    return DropFragments(UnitsFromParagraphs(paragraphs));
}

}  // namespace clinicavt::guidance
