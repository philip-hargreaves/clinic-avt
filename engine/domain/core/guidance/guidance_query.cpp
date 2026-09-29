#include "core/guidance/guidance_query.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "core/common/strings.hpp"

namespace clinicavt::guidance {

namespace {

bool IsAbbreviation(std::string_view token) {
    static const char* const kWords[] = {"e.g",    "i.e", "dr", "mr", "mrs", "ms",  "prof", "vs",
                                         "approx", "etc", "no", "hx", "pt",  "rx",  "mg",   "mcg",
                                         "ml",     "kg",  "cm", "mm", "bd",  "tds", "od",   "prn",
                                         "st",     "ca",  "cf", "wk", "wks", "yr",  "yrs",  "mth"};
    const auto lower = strings::Lower(token);
    if (lower.size() == 1 && std::isalpha(static_cast<unsigned char>(lower[0]))) return true;
    for (const char* k : kWords) {
        if (lower == k) return true;
    }
    return false;
}

std::string_view TokenBefore(std::string_view text, std::size_t pos) {
    std::size_t start = pos;
    while (start > 0 && !std::isspace(static_cast<unsigned char>(text[start - 1]))) --start;
    return text.substr(start, pos - start);
}

}  // namespace

bool OpensSentence(unsigned char c) {
    return std::isupper(c) || std::isdigit(c) || c == '"' || c == '\'' || c == '(';
}

std::vector<std::string> SplitSentences(std::string_view note) {
    std::vector<std::string> out;
    auto flush = [&](std::size_t from, std::size_t to) {
        const auto piece = strings::Trim(note.substr(from, to - from));
        if (strings::WordCount(piece) >= kMinSentenceWords) out.emplace_back(piece);
    };
    std::size_t start = 0;
    for (std::size_t i = 0; i < note.size(); ++i) {
        const char c = note[i];
        if (c == '\n') {
            flush(start, i);
            start = i + 1;
            continue;
        }
        if (c != '.' && c != '!' && c != '?') continue;
        std::size_t j = i + 1;
        while (j < note.size() && (note[j] == ' ' || note[j] == '\t' || note[j] == '\r')) ++j;
        if (j == i + 1 || j >= note.size()) continue;
        if (!OpensSentence(static_cast<unsigned char>(note[j]))) continue;
        if (c == '.' && IsAbbreviation(TokenBefore(note, i))) continue;
        flush(start, i + 1);
        start = j;
    }
    flush(start, note.size());
    return out;
}

bool IsExcluded(std::string_view sentence) {
    const auto s = strings::Lower(strings::Trim(sentence));
    static const char* const kOpenings[] = {
        "no ",      "nil ",         "not ",       "denies",         "denied",    "never ",
        "without ", "negative for", "no history", "family history", "fh:",       "fh ",
        "fhx",      "if ",          "unless ",    "should she",     "should he", "in case"};
    for (const char* k : kOpenings) {
        if (s.starts_with(k)) return true;
    }
    static const char* const kPhrases[] = {" mother had",    " father had",    " mother has",
                                           " father has",    " sister had",    " brother had",
                                           "grandmother",    "grandfather",    "family history of",
                                           "no evidence of", "were to develop"};
    for (const char* k : kPhrases) {
        if (strings::Contains(s, k)) return true;
    }
    return false;
}

std::vector<std::string> SubQueries(std::string_view note) {
    std::vector<std::string> out;
    for (auto& sentence : SplitSentences(note)) {
        if (!IsExcluded(sentence)) out.push_back(std::move(sentence));
    }
    const auto whole = strings::Trim(note);
    if (strings::WordCount(whole) >= kMinSentenceWords &&
        std::find(out.begin(), out.end(), whole) == out.end()) {
        out.emplace_back(whole);
    }
    return out;
}

}  // namespace clinicavt::guidance
