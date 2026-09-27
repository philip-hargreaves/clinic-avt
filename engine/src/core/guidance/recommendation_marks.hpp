#pragma once

#include <string>
#include <string_view>

namespace clinicavt::guidance {

// The leading dotted number of a paragraph ("1.2.3"), or empty
std::string LeadingNumber(const std::string& paragraph);

// "Recommendation 12", "recommendation 3a", "1.2", "1.2.3 Offer": a paragraph
// that opens a numbered recommendation
bool StartsRecommendation(const std::string& paragraph);

// How a document numbers its recommendations: "1.2.3", "(iv)", "(3)", "3.",
// "Recommendation 3a" or "R3"
enum class Scheme { kNone, kDotted, kRoman, kBracketed, kNumbered, kWord, kLetterR };

// Every scheme with a mark. Detection prefers the earlier on a tie
inline constexpr Scheme kMarkSchemes[] = {Scheme::kDotted,   Scheme::kRoman, Scheme::kBracketed,
                                          Scheme::kNumbered, Scheme::kWord,  Scheme::kLetterR};

// A scheme is the document's own once this many paragraphs open with it
inline constexpr int kSchemeMarks = 2;

// A document closes its recommendations with a token, a grade or a NICE date
// tag, once this many paragraphs end with one
inline constexpr int kTokenParagraphs = 2;

// The mark that opens `text` under `scheme`, empty when there is none
std::string_view MarkOf(std::string_view text, Scheme scheme);

// A line that is only the wrapped tail of a grade token, "SoA 100%)." left when
// "(GRADE 1B, SoA 100%)" breaks across a line in a narrow table column. No
// recommendation opens with its own grade, so such a line rejoins the one above
bool IsGradeTail(std::string_view text);

// "(GRADE 1C, SoA 98%)", "GRADE 2C", "SOR: 97% (range 88-100%)" or "[2017]" at
// the end of a recommendation, allowing a short tail after the token
bool EndsWithToken(std::string_view text);

}  // namespace clinicavt::guidance
