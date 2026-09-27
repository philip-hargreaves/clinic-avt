#include "core/guidance/recommendation_marks.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace clinicavt::guidance {

namespace {

std::size_t Digits(std::string_view s, std::size_t at) {
    std::size_t n = 0;
    while (at + n < s.size() && std::isdigit(static_cast<unsigned char>(s[at + n]))) ++n;
    return n;
}

// s opens with the lower-case prefix, in any case, and runs on past it
bool OpensWithNoCase(std::string_view s, std::string_view prefix) {
    if (s.size() <= prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) != prefix[i]) return false;
    }
    return true;
}

// A mark ends the text or gives way to a space or colon
bool MarkEnds(std::string_view s, std::size_t at) {
    return at >= s.size() || s[at] == ' ' || s[at] == ':';
}

std::size_t RomanMark(std::string_view s) {
    if (s.empty() || s[0] != '(') return 0;
    std::size_t i = 1;
    while (i < s.size() && std::string_view("ivxIVX").find(s[i]) != std::string_view::npos) ++i;
    if (i == 1 || i >= s.size() || s[i] != ')' || !MarkEnds(s, i + 1)) return 0;
    return i + 1;
}

std::size_t BracketedMark(std::string_view s) {
    if (s.empty() || s[0] != '(') return 0;
    const auto n = Digits(s, 1);
    if (n == 0 || n > 2 || 1 + n >= s.size() || s[1 + n] != ')' || !MarkEnds(s, 2 + n)) return 0;
    return 2 + n;
}

std::size_t NumberedMark(std::string_view s) {
    const auto n = Digits(s, 0);
    if (n == 0 || n > 2 || n >= s.size() || s[n] != '.' || !MarkEnds(s, n + 1)) return 0;
    return n + 1;
}

// "Recommendation 3", "Recommendation 2a", any case
std::size_t WordMark(std::string_view s) {
    static constexpr std::string_view kWord = "recommendation ";
    if (!OpensWithNoCase(s, kWord)) return 0;
    auto end = kWord.size() + Digits(s, kWord.size());
    if (end == kWord.size()) return 0;
    if (end < s.size() && std::islower(static_cast<unsigned char>(s[end]))) ++end;
    return MarkEnds(s, end) ? end : 0;
}

std::size_t LetterRMark(std::string_view s) {
    if (s.empty() || s[0] != 'R') return 0;
    const auto n = Digits(s, 1);
    if (n == 0 || n > 2) return 0;
    auto end = 1 + n;
    if (end < s.size() && std::islower(static_cast<unsigned char>(s[end]))) ++end;
    return MarkEnds(s, end) ? end : 0;
}

bool GradeAt(std::string_view s, std::size_t at, std::size_t& end) {
    if (s.substr(at).starts_with("GRADE ") && at + 6 < s.size() &&
        std::isdigit(static_cast<unsigned char>(s[at + 6]))) {
        end = at + 7;
        if (end < s.size() && std::isupper(static_cast<unsigned char>(s[end]))) ++end;
        return true;
    }
    for (const std::string_view key : {"SoA", "SOA", "SOR", "SoR", "SOE", "SoE"}) {
        if (!s.substr(at).starts_with(key)) continue;
        auto i = at + key.size();
        if (i < s.size() && s[i] == ':') ++i;
        while (i < s.size() && s[i] == ' ') ++i;
        const auto n = Digits(s, i);
        if (n == 0 || i + n >= s.size() || s[i + n] != '%') return false;
        end = i + n + 1;
        return true;
    }
    return false;
}

// "[2017]" or "[2009, amended 2018]" closing a NICE recommendation
bool DateTagAt(std::string_view s, std::size_t at, std::size_t& end) {
    if (s[at] != '[' || Digits(s, at + 1) != 4) return false;
    const auto close = s.find(']', at);
    if (close == std::string_view::npos) return false;
    const auto inside = s.substr(at + 5, close - at - 5);
    if (!inside.empty() && !inside.starts_with(", amended ")) return false;
    end = close + 1;
    return true;
}

}  // namespace

std::string LeadingNumber(const std::string& paragraph) {
    const std::size_t i = Digits(paragraph, 0);
    if (i == 0 || i >= paragraph.size() || paragraph[i] != '.') return "";
    std::size_t end = i;
    while (end < paragraph.size() && paragraph[end] == '.') {
        const std::size_t k = end + 1 + Digits(paragraph, end + 1);
        if (k == end + 1) break;
        end = k;
    }
    if (end < paragraph.size() && std::isalnum(static_cast<unsigned char>(paragraph[end])))
        return "";
    return paragraph.substr(0, end);
}

bool StartsRecommendation(const std::string& paragraph) {
    if (!LeadingNumber(paragraph).empty()) return true;
    static constexpr std::string_view kWord = "recommendation";
    if (!OpensWithNoCase(paragraph, kWord)) return false;
    std::size_t i = kWord.size();
    if (!std::isspace(static_cast<unsigned char>(paragraph[i]))) return false;
    while (i < paragraph.size() && std::isspace(static_cast<unsigned char>(paragraph[i]))) ++i;
    std::size_t digits = i;
    while (digits < paragraph.size() &&
           std::isdigit(static_cast<unsigned char>(paragraph[digits]))) {
        ++digits;
    }
    if (digits == i) return false;
    if (digits < paragraph.size() && std::isalpha(static_cast<unsigned char>(paragraph[digits]))) {
        ++digits;
    }
    return digits >= paragraph.size() ||
           !std::isalnum(static_cast<unsigned char>(paragraph[digits]));
}

std::string_view MarkOf(std::string_view text, Scheme scheme) {
    std::size_t length = 0;
    switch (scheme) {
        case Scheme::kDotted:
            length = LeadingNumber(std::string(text)).size();
            break;
        case Scheme::kRoman:
            length = RomanMark(text);
            break;
        case Scheme::kBracketed:
            length = BracketedMark(text);
            break;
        case Scheme::kNumbered:
            length = NumberedMark(text);
            break;
        case Scheme::kWord:
            length = WordMark(text);
            break;
        case Scheme::kLetterR:
            length = LetterRMark(text);
            break;
        case Scheme::kNone:
            break;
    }
    return text.substr(0, length);
}

bool IsGradeTail(std::string_view text) {
    std::size_t at = 0;
    while (at < text.size() && (text[at] == ' ' || text[at] == '(')) ++at;
    std::size_t end = 0;
    return GradeAt(text, at, end);
}

bool EndsWithToken(std::string_view text) {
    constexpr std::size_t kTail = 30;
    std::size_t last = std::string_view::npos;
    for (std::size_t at = 0; at < text.size(); ++at) {
        std::size_t end = 0;
        if (GradeAt(text, at, end) || DateTagAt(text, at, end)) last = end;
    }
    return last != std::string_view::npos && text.size() - last <= kTail;
}

}  // namespace clinicavt::guidance
