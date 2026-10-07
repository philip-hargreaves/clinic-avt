#include "core/note/note_label.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace clinicavt::note {
namespace {

TEST(NoteLabel, LabelFromTakesTheFirstSentenceCappedAtAWord) {
    const std::vector<std::pair<std::string, std::string>> rows = {
        {"Swelling of the left elbow for a week. Not painful.",
         "Swelling of the left elbow for a week"},
        {"Is it infected? No.", "Is it infected"},
        // A dose's decimal point does not end the sentence
        {"Ibuprofen 1.5 g daily was advised. Review in a week.",
         "Ibuprofen 1.5 g daily was advised"},
        {"Temperature 37.8C", "Temperature 37.8C"},
        {"Subjective:\n  Swollen   left\r\nelbow. Objective: warm.",
         "Subjective: Swollen left elbow"},
        {"   \n  ", ""},
        {"", ""},
        // Over the 80-character cap, so cut at the last whole word
        {"The patient, a 53-year-old male, presents with a swelling on the left elbow noticed "
         "about a week ago with no injury and no pain",
         "The patient, a 53-year-old male, presents with a swelling on the left elbow"},
    };
    for (const auto& [text, label] : rows) {
        EXPECT_EQ(LabelFrom(text), label) << text;
    }
    EXPECT_EQ(LabelFrom("abcdefghij", 5), "abcde") << "no word boundary: hard cut";
}

TEST(NoteLabel, SanitiseKeepsAUsableTitleOrNothing) {
    const std::vector<std::pair<std::string, std::string>> rows = {
        {"Elbow swelling", "Elbow swelling"},
        {"  \"Left elbow swelling.\"  ", "Left elbow swelling"},
        {"'Chest pain follow-up',", "Chest pain follow-up"},
        {"Diarrhoea and vomiting\nThe patient also...", "Diarrhoea and vomiting"},
        {"", ""},
        {"  \"...\"  ", ""},
        {"\n\n", ""},
    };
    for (const auto& [title, kept] : rows) {
        EXPECT_EQ(SanitiseLabel(title), kept) << title;
    }
    EXPECT_LE(SanitiseLabel(std::string(200, 'a')).size(), 60u) << "a rambling title is capped";
}

}  // namespace
}  // namespace clinicavt::note
