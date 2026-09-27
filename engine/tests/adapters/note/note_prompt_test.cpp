#include "adapters/note/note_prompt.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace clinicavt::note {
namespace {

TEST(NotePrompt, LoadsVerbatimAndAMissingFileIsLoud) {
    const auto path = std::filesystem::temp_directory_path() / "clinicavt-note-prompt-test.md";
    std::ofstream(path) << "Write the note.\n\nTRANSCRIPT:\n";
    EXPECT_EQ(LoadPrompt(path), "Write the note.\n\nTRANSCRIPT:\n");
    std::filesystem::remove(path);

    EXPECT_THROW(LoadPrompt("C:/does/not/exist/prompt.md"), std::runtime_error);
}

// Exactly what the note model reads
TEST(NotePrompt, TranscriptBlockIsUpperCasedRolesOnePerLine) {
    const std::vector<asr::Turn> turns{{0, 100, "doctor", "How long has it hurt?"},
                                       {100, 100, "patient", "About a week."},
                                       {200, 100, "", "Okay."}};
    EXPECT_EQ(TranscriptBlock(turns),
              "DOCTOR: How long has it hurt?\n"
              "PATIENT: About a week.\n"
              "SPEAKER: Okay.\n");
}

}  // namespace
}  // namespace clinicavt::note
