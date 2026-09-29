#pragma once

#include <algorithm>
#include <regex>
#include <string>
#include <string_view>

namespace clinicavt::note {

// Removes direct identifiers from a case summary. Ages become a decade and dates become "recently".
// NHS numbers, postcodes and phone numbers are removed. Contextual identifiers are left to the
// clinician
namespace scrub_detail {

inline std::string Decade(int age) {
    if (age < 10) return "under ten";
    if (age < 20) return "in their teens";
    static constexpr std::string_view kTens[] = {"twenties", "thirties",  "forties",  "fifties",
                                                 "sixties",  "seventies", "eighties", "nineties"};
    const auto index = std::min(age / 10 - 2, 7);
    return "in their " + std::string(kTens[index]);
}

// "a 53-year-old male" -> "a male in their fifties". With no noun after
// it, "a patient in their fifties"
inline std::string ScrubAgeNouns(std::string text) {
    static const std::regex kAgeNoun(
        R"((\b(?:a|an|the)\s+)?\b(\d{1,3})(?:[ -]?(?:year|yr)s?[ -]old|\s*y/o|\s*yo)\b(?:\s+(male|female|man|woman|boy|girl|patient|child|adult|gentleman|lady))?)",
        std::regex::icase);
    std::smatch m;
    std::string out;
    while (std::regex_search(text, m, kAgeNoun)) {
        const auto age = std::stoi(m[2]);
        const std::string article = m[1].matched ? m[1].str() : "";
        const std::string noun = m[3].matched ? m[3].str() : (article.empty() ? "" : "patient");
        out += m.prefix().str() + article + noun + (noun.empty() ? "" : " ") + Decade(age);
        text = m.suffix().str();
    }
    return out + text;
}

// "aged 53", "53 years old" -> "in their fifties"
inline std::string ScrubAgePhrases(std::string text) {
    static const std::regex kAge(R"(\b(?:aged?|age of)\s+(\d{1,3})\b|\b(\d{1,3})\s+years?\s+old\b)",
                                 std::regex::icase);
    std::smatch m;
    std::string out;
    while (std::regex_search(text, m, kAge)) {
        const auto age = std::stoi(m[1].matched ? m[1].str() : m[2].str());
        out += m.prefix().str() + Decade(age);
        text = m.suffix().str();
    }
    return out + text;
}

inline std::string ScrubDates(std::string text) {
    static const std::regex kNumeric(
        R"(\b\d{1,2}[/.-]\d{1,2}[/.-]\d{2,4}\b|\b\d{4}-\d{2}-\d{2}\b)");
    // A month counts as a date only with a day or year beside it: "in December" passes, "decision"
    // never matches
    static const std::regex kWritten(
        R"(\b(?:\d{1,2}(?:st|nd|rd|th)?\s+(?:of\s+)?(?:January|February|March|April|May|June|July|August|September|October|November|December|Jan|Feb|Mar|Apr|Jun|Jul|Aug|Sept|Sep|Oct|Nov|Dec)\b\.?(?:,?\s+\d{4})?|(?:January|February|March|April|May|June|July|August|September|October|November|December|Jan|Feb|Mar|Apr|Jun|Jul|Aug|Sept|Sep|Oct|Nov|Dec)\b\.?\s+\d{1,2}(?:st|nd|rd|th)?(?:,?\s+\d{4})?\b|(?:January|February|March|April|May|June|July|August|September|October|November|December|Jan|Feb|Mar|Apr|Jun|Jul|Aug|Sept|Sep|Oct|Nov|Dec)\b\.?\s+\d{4}\b))");
    text = std::regex_replace(text, kNumeric, "recently");
    text = std::regex_replace(text, kWritten, "recently");
    static const std::regex kOn(R"(\b(?:on|in|since|from)\s+recently\b)", std::regex::icase);
    return std::regex_replace(text, kOn, "recently");
}

inline std::string ScrubNumbers(std::string text) {
    static const std::regex kNhs(R"(\b\d{3}[ -]?\d{3}[ -]?\d{4}\b)");
    static const std::regex kPhone(R"(\b0\d{2,4}[ -]?\d{3,4}[ -]?\d{3,4}\b)");
    static const std::regex kPostcode(R"(\b[A-Z]{1,2}\d[A-Z\d]?\s*\d[A-Z]{2}\b)");
    text = std::regex_replace(text, kNhs, "");
    text = std::regex_replace(text, kPhone, "");
    return std::regex_replace(text, kPostcode, "");
}

inline std::string Tidy(std::string text) {
    static const std::regex kSpaces(R"([ \t]{2,})");
    static const std::regex kBeforePunct(R"( +([,.;:]))");
    text = std::regex_replace(text, kSpaces, " ");
    text = std::regex_replace(text, kBeforePunct, "$1");
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
    return text;
}

}  // namespace scrub_detail

inline std::string ScrubSummary(std::string_view input) {
    std::string text(input);
    text = scrub_detail::ScrubAgeNouns(std::move(text));
    text = scrub_detail::ScrubAgePhrases(std::move(text));
    text = scrub_detail::ScrubDates(std::move(text));
    text = scrub_detail::ScrubNumbers(std::move(text));
    return scrub_detail::Tidy(std::move(text));
}

}  // namespace clinicavt::note
