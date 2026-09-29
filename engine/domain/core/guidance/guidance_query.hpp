#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace clinicavt::guidance {

// Sentence splitting must match the evaluation harness so both build the same candidate lists
inline constexpr int kMinSentenceWords = 3;

// A newline always ends a sentence. A full stop, exclamation mark or question
// mark ends one when whitespace and a capital, digit, quote or bracket follow
// and the word before is not an abbreviation. Fragments under
// kMinSentenceWords are dropped
std::vector<std::string> SplitSentences(std::string_view note);

// True for negated findings, family history and hypotheticals, since guidance for them would target
// a condition the patient doesn't have. Only openings and unambiguous phrases match
bool IsExcluded(std::string_view sentence);

// The sentences that pass the filter, then the whole note as one more query
std::vector<std::string> SubQueries(std::string_view note);

// True for a capital, digit, quote or bracket
bool OpensSentence(unsigned char c);

}  // namespace clinicavt::guidance
