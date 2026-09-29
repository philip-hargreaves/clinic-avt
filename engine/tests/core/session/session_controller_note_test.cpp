#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "core/session/session_controller.hpp"
#include "session_test_harness.hpp"

namespace clinicavt::session {
namespace {

using namespace test_harness;

TEST(SessionController, StopWritesTheNoteThenTheSheetThenTheTitle) {
    Rig rig;
    rig.diariser.clusters = 2;
    rig.diariser.similarities = {0.2, 0.8};
    rig.writer.patient = true;
    rig.writer.block = true;  // holds the note so the print can be seen waiting for it
    {
        auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});
        controller.SetNoteOptions({note::NoteStyle::kSoap, note::NoteDetail::kConcise});
        ASSERT_TRUE(controller.Start());
        ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
        controller.Stop();

        ASSERT_TRUE(rig.events.WaitUntil([&] { return !rig.events.note_partials.empty(); }));
        EXPECT_EQ(rig.diariser.accruals.load(), 0) << "the print waits for an accepted note";
        EXPECT_FALSE(controller.Running());
        EXPECT_TRUE(controller.Busy()) << "capture is over but the note is still being written";
        rig.writer.block = false;
        ASSERT_TRUE(rig.events.WaitForNote());
        EXPECT_EQ(controller.LastFinalised(), "s1");
        EXPECT_TRUE(WaitFor([&] { return !controller.Busy(); }))
            << "free once note and sheet are done";
    }  // the controller joins the lane, so the sheet and the title are done

    EXPECT_EQ(rig.events.note_partials,
              (std::vector<std::string>{"the clinica", "the clinical note"}));
    EXPECT_EQ(rig.events.note_ready, "the clinical note");
    EXPECT_TRUE(rig.events.note_failed.empty());
    EXPECT_EQ(rig.events.note_saved_session, "s1");
    EXPECT_EQ(rig.events.note_saved_text, "the clinical note");
    EXPECT_EQ(rig.events.note_saved_revision, 1) << "the note as stored, not as generated";
    ASSERT_EQ(rig.writer.calls.size(), 1u);
    EXPECT_FALSE(rig.writer.calls[0].empty()) << "the writer gets the transcript";
    EXPECT_EQ(rig.writer.last_options.style, note::NoteStyle::kSoap);
    EXPECT_EQ(rig.writer.last_options.detail, note::NoteDetail::kConcise);
    EXPECT_EQ(rig.store.note, "the clinical note");
    EXPECT_EQ(rig.store.note_style, "soap");
    EXPECT_EQ(rig.store.note_detail, "concise");

    EXPECT_EQ(rig.writer.patient_input, "the clinical note")
        << "the sheet is written from the note";
    EXPECT_EQ(rig.events.patient_partials, (std::vector<std::string>{"Your appointment"}));
    EXPECT_EQ(rig.events.patient_ready, "the patient sheet");
    EXPECT_EQ(rig.store.patient, "the patient sheet");
    EXPECT_EQ(rig.store.Calls(), (std::vector<std::string>{"begin s1", "replace s1", "finalise s1",
                                                           "note s1", "patient s1"}));

    EXPECT_EQ(rig.writer.label_calls.load(), 1);
    EXPECT_EQ(rig.store.label, "Elbow swelling") << "the model's title, sanitised";
    EXPECT_FALSE(rig.store.label_typed);

    EXPECT_EQ(rig.diariser.accruals.load(), 1) << "learned once the note was accepted";
    EXPECT_EQ(rig.diariser.accrued_cluster, 1);
    EXPECT_TRUE(rig.events.storage_faults.empty());
}

TEST(SessionController, NoteLaneFailuresNeverCostWhatCameBefore) {
    struct Row {
        const char* name;
        std::function<void(Rig&)> arrange;
        std::function<void(Rig&)> check;
    };
    const std::vector<Row> rows = {
        {"the writer fails",
         [](Rig& rig) {
             rig.writer.fail = true;
             rig.writer.patient = true;
         },
         [](Rig& rig) {
             EXPECT_EQ(rig.events.note_failed, "generation failed");
             EXPECT_TRUE(rig.events.note_ready.empty());
             EXPECT_TRUE(rig.store.note.empty());
             EXPECT_TRUE(rig.writer.patient_input.empty()) << "no sheet without a note";
             EXPECT_EQ(rig.writer.label_calls.load(), 0);
         }},
        {"the sheet fails",
         [](Rig& rig) {
             rig.writer.patient = true;
             rig.writer.fail_patient = true;
         },
         [](Rig& rig) {
             EXPECT_EQ(rig.events.patient_failed, "patient generation failed");
             EXPECT_TRUE(rig.events.patient_ready.empty());
             EXPECT_EQ(rig.events.note_ready, "the clinical note");
             EXPECT_EQ(rig.store.note, "the clinical note");
             EXPECT_TRUE(rig.store.patient.empty());
         }},
        {"the store refuses documents", [](Rig& rig) { rig.store.refuse_documents = true; },
         [](Rig& rig) {
             EXPECT_EQ(rig.events.note_ready, "the clinical note");
             EXPECT_TRUE(rig.events.note_saved_session.empty())
                 << "nothing stored, nothing to search";
             EXPECT_TRUE(rig.store.note.empty());
             ASSERT_FALSE(rig.events.storage_faults.empty()) << "a full disk reaches the shell";
             EXPECT_EQ(rig.events.storage_faults.front(), "disk full");
         }},
        {"the work after the note throws", [](Rig& rig) { rig.events.note_saved_throws = true; },
         [](Rig& rig) {
             EXPECT_EQ(rig.events.note_ready, "the clinical note");
             EXPECT_TRUE(rig.events.note_failed.empty());
             EXPECT_EQ(rig.store.note, "the clinical note");
         }},
        {"the title is rejected",
         [](Rig& rig) { rig.writer.label_result = "  \"...\"  "; },  // nothing survives
         [](Rig& rig) {
             EXPECT_EQ(rig.writer.label_calls.load(), 1);
             EXPECT_EQ(rig.store.label, "")
                 << "no title beats a bad title; the list shows the date";
         }},
        {"a note-only writer", [](Rig&) {},
         [](Rig& rig) {
             EXPECT_EQ(rig.events.note_ready, "the clinical note");
             EXPECT_TRUE(rig.events.patient_partials.empty()) << "the sheet lane never runs";
             EXPECT_TRUE(rig.writer.patient_input.empty());
         }},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        Rig rig;
        row.arrange(rig);
        {
            auto controller = rig.Make(Script::kCompleteAfterAudio, {.writer = true});
            ASSERT_TRUE(controller.Start());
            controller.Stop();
            ASSERT_TRUE(rig.events.WaitForNote());
        }  // joins the lane
        row.check(rig);
    }
}

// The title is written after the documents. A consultation opened meanwhile
// is not refused for it
TEST(SessionController, OpenIsNotRefusedWhileTheTitleIsWritten) {
    Rig rig;
    rig.store.turns = {
        {0, 16000 * 30, "doctor", "a stored consultation with enough words to note"}};
    rig.writer.block_label = true;
    auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});

    ASSERT_TRUE(controller.Start());
    controller.Stop();
    ASSERT_TRUE(rig.events.WaitForNote());
    ASSERT_TRUE(WaitFor([&] { return rig.writer.label_calls.load() == 1; }))
        << "the title is being written";

    EXPECT_TRUE(controller.Open("past"));

    rig.writer.block_label = false;
    ASSERT_TRUE(WaitFor([&] {
        const std::lock_guard<std::mutex> lock(rig.store.mutex);
        return !rig.store.label.empty();
    }));
    EXPECT_EQ(rig.store.label, "Elbow swelling");
}

TEST(SessionController, AReviewedSessionRegeneratesItsNoteSheetAndSummary) {
    Rig rig;
    rig.store.turns = {
        {0, 16000 * 30, "doctor", "a stored consultation with enough words to note"}};
    rig.store.label = "Elbow swelling";
    rig.store.label_typed = true;
    rig.writer.patient = true;
    const std::string edited = "the note as the clinician edited it";
    {
        auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});
        // A call is refused while the lane finishes the previous write
        const auto accepted = [](auto call) { return WaitFor(call); };

        // Any stored session can be summarised, no review needed
        EXPECT_FALSE(controller.WriteSummary("past")) << "no note, nothing to summarise";
        rig.store.note = edited;
        ASSERT_TRUE(controller.WriteSummary("past"));
        ASSERT_TRUE(rig.events.WaitForSummary());
        EXPECT_EQ(rig.events.summary_session, "past");
        EXPECT_EQ(rig.events.summary_text,
                  "A patient in their forties presented with a swollen elbow.");
        EXPECT_EQ(rig.writer.summary_input, edited);
        EXPECT_EQ(rig.store.summary, rig.events.summary_text);
        {
            const std::lock_guard<std::mutex> lock(rig.events.mutex);
            rig.events.summary_done = false;
        }
        rig.writer.fail_summary = true;
        ASSERT_TRUE(accepted([&] { return controller.WriteSummary("past"); }));
        ASSERT_TRUE(rig.events.WaitForSummary());
        EXPECT_EQ(rig.events.summary_failed, "summary generation failed");

        EXPECT_FALSE(controller.RegeneratePatient()) << "nothing open yet";
        ASSERT_TRUE(accepted([&] { return controller.Open("past"); }));
        EXPECT_EQ(controller.LastFinalised(), "past");

        ASSERT_TRUE(accepted([&] { return controller.RegeneratePatient(); }));
        ASSERT_TRUE(rig.events.WaitForPatient());
        EXPECT_EQ(rig.writer.patient_input, edited)
            << "the sheet regenerates from the stored note, edits included";
        EXPECT_EQ(rig.store.patient, "the patient sheet");

        ASSERT_TRUE(accepted([&] {
            return controller.RegenerateNote({note::NoteStyle::kSoap, note::NoteDetail::kConcise});
        }));
        ASSERT_TRUE(rig.events.WaitForNote());
        EXPECT_EQ(rig.store.note, "the clinical note");
        EXPECT_EQ(rig.store.note_style, "soap");
        const auto calls = rig.store.Calls();
        EXPECT_NE(std::find(calls.begin(), calls.end(), "note past"), calls.end())
            << "the note is stored against the opened session";

        controller.Close();
        EXPECT_TRUE(controller.LastFinalised().empty()) << "leaving ends the review";
        EXPECT_FALSE(
            controller.RegenerateNote({note::NoteStyle::kProse, note::NoteDetail::kConcise}))
            << "closed";
        EXPECT_FALSE(controller.RegeneratePatient()) << "closed";
    }  // joins the lane, so the title step has run

    EXPECT_EQ(rig.store.label, "Elbow swelling");
    EXPECT_TRUE(rig.store.label_typed);
    EXPECT_EQ(rig.writer.label_calls.load(), 0) << "a typed label is never regenerated";
}

TEST(SessionController, RegenerateAndOpenAreRefusedWhenTheyCannotRun) {
    {
        Rig rig;
        rig.store.turns = {{0, 16000, "doctor", "words"}};
        auto controller = rig.Make(Script::kStreamUntilStopped);
        ASSERT_TRUE(controller.Open("past"));
        EXPECT_FALSE(
            controller.RegenerateNote({note::NoteStyle::kProse, note::NoteDetail::kConcise}))
            << "no writer: refused, not crashed";
    }

    Rig rig;
    auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});
    EXPECT_FALSE(controller.RegenerateNote({})) << "nothing finalised yet";

    rig.store.refuse_read_turns = true;
    EXPECT_FALSE(controller.Open("nope")) << "an unknown session";
    rig.store.refuse_read_turns = false;

    ASSERT_TRUE(controller.Open("past"));
    ASSERT_TRUE(controller.Start());
    EXPECT_FALSE(controller.Open("past")) << "not while recording";
    EXPECT_FALSE(controller.RegenerateNote({})) << "not while recording";
    controller.Stop();
    ASSERT_TRUE(rig.events.WaitForNote());
    EXPECT_EQ(controller.LastFinalised(), "s1") << "a finalise sets its own target";

    rig.events.ResetNote();
    rig.store.turns = {{0, 16000, "doctor", "words"}};  // regenerate needs a stored transcript
    ASSERT_TRUE(WaitFor([&] {
        return controller.RegenerateNote({note::NoteStyle::kSoap, note::NoteDetail::kConcise});
    }));
    ASSERT_TRUE(rig.events.WaitForNote());
    ASSERT_EQ(rig.writer.calls.size(), 2u);
    EXPECT_EQ(rig.writer.last_options.style, note::NoteStyle::kSoap);
    EXPECT_EQ(rig.writer.last_options.detail, note::NoteDetail::kConcise);
    EXPECT_EQ(rig.store.note, "the clinical note") << "the regenerated note is re-saved";

    controller.Close();
    EXPECT_EQ(controller.LastFinalised(), "s1")
        << "Record closed the review, so leaving keeps the finalised session";
}

TEST(SessionController, AThinOrMissingTranscriptIsRefusedWithoutAskingTheModel) {
    struct Row {
        const char* name;
        bool diarise_fails;
        std::vector<std::string> calls;
        std::string refusal;
    };
    const std::vector<Row> rows = {
        {"a thin transcript",
         false,
         {"begin s1", "replace s1", "finalise s1"},
         "5 words; a note needs at least 25"},
        // The session is never lost to a diarisation failure
        {"diarisation failed, so no transcript",
         true,
         {"begin s1", "finalise s1"},
         "0 words; a note needs at least 25"},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        Rig rig;
        rig.diariser.fail_diarise = row.diarise_fails;
        rig.writer.patient = true;
        auto controller = rig.Make(Script::kStreamUntilStopped,
                                   {.writer = true, .min_note_words = NoteLane::kMinNoteWords});

        ASSERT_TRUE(controller.Start());
        ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
        controller.Stop();
        ASSERT_TRUE(rig.events.WaitForNote());

        EXPECT_EQ(rig.store.Calls(), row.calls);
        EXPECT_EQ(controller.LastFinalised(), "s1");
        EXPECT_GE(rig.diariser.discards, 1);
        EXPECT_TRUE(rig.writer.calls.empty()) << "the model must never see a transcript this thin";
        EXPECT_EQ(rig.events.note_refused, row.refusal);
        EXPECT_FALSE(rig.events.note_refused_overridable)
            << "insisting would make the model fabricate";
        EXPECT_TRUE(rig.events.note_ready.empty());
        EXPECT_TRUE(rig.events.patient_ready.empty()) << "no sheet without a note";
        EXPECT_TRUE(rig.events.note_failed.empty()) << "a thin recording is not an error state";
        EXPECT_TRUE(rig.store.note.empty());
    }
}

TEST(SessionController, ANoteStillWritingIsCancelledByCancelOrDestruction) {
    {
        Rig rig;
        auto controller = rig.Make(Script::kCompleteAfterAudio, {.writer = true});
        ASSERT_TRUE(controller.Start());
        controller.Cancel();
        EXPECT_TRUE(rig.writer.calls.empty()) << "a cancel writes no note";
        EXPECT_TRUE(rig.events.note_partials.empty());
    }

    Rig rig;
    rig.writer.block = true;
    {
        auto controller = rig.Make(Script::kCompleteAfterAudio, {.writer = true});
        ASSERT_TRUE(controller.Start());
        controller.Stop();
        // Destruction must catch the write while it is in flight
        ASSERT_TRUE(rig.events.WaitUntil([&] { return !rig.events.note_partials.empty(); }));
    }
    EXPECT_TRUE(rig.writer.cancelled.load());
    EXPECT_EQ(rig.events.note_ready, "interrupted") << "an interrupted write returns what it had";
}

TEST(SessionController, ARefusedRecordingYieldsNothingAndIsErasedAtClose) {
    Rig rig;
    rig.diariser.clusters = 2;
    rig.diariser.similarities = {0.9, 0.3};
    rig.writer.patient = true;
    rig.writer.result = "NOT A CONSULTATION: a cooking video with one speaker";
    auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
    controller.Stop();
    ASSERT_TRUE(rig.events.WaitForNote());
    {
        const std::lock_guard<std::mutex> lock(rig.events.mutex);
        EXPECT_EQ(rig.events.note_refused, "a cooking video with one speaker");
        EXPECT_TRUE(rig.events.note_refused_overridable) << "the clinician may insist";
        EXPECT_TRUE(rig.events.note_ready.empty()) << "no note reached the shell";
        EXPECT_TRUE(rig.events.note_partials.empty()) << "the refusal never streamed as a note";
    }
    EXPECT_EQ(rig.writer.patient_input, "") << "no sheet";
    EXPECT_EQ(rig.writer.label_calls.load(), 0) << "no title";
    EXPECT_EQ(rig.diariser.accruals.load(), 0) << "the print learned nothing";
    for (const auto& call : rig.store.Calls()) {
        EXPECT_EQ(call.find("note"), std::string::npos) << call;
    }

    controller.Close();
    const auto calls = rig.store.Calls();
    EXPECT_NE(std::find(calls.begin(), calls.end(), "delete s1"), calls.end())
        << "a refused recording is not kept";
    EXPECT_TRUE(controller.LastFinalised().empty());
}

TEST(SessionController, AnInsistedRewriteIsDeliveredAndTheSessionKept) {
    Rig rig;
    rig.diariser.clusters = 2;
    rig.writer.result = "NOT A CONSULTATION: a cooking video";
    auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
    controller.Stop();
    ASSERT_TRUE(rig.events.WaitForNote());
    ASSERT_FALSE(rig.events.note_refused.empty());

    // With refusal off, the lane passes the model output through
    rig.events.ResetNote();
    rig.writer.result = "NOT A CONSULTATION: the model still says so";
    note::NoteOptions confirmed;
    confirmed.confirmed = true;
    ASSERT_TRUE(WaitFor([&] { return controller.RegenerateNote(confirmed); }));
    ASSERT_TRUE(rig.events.WaitForNote());
    {
        const std::lock_guard<std::mutex> lock(rig.events.mutex);
        EXPECT_TRUE(rig.events.note_refused.empty());
        EXPECT_EQ(rig.events.note_ready, "NOT A CONSULTATION: the model still says so");
    }

    controller.Close();
    for (const auto& call : rig.store.Calls()) {
        EXPECT_NE(call, "delete s1") << "the clinician insisted, so the session stays";
    }
}

// Restored consultations can lack a transcript. A reviewed note must still be kept
TEST(SessionController, AReviewedSessionIsNeverRewrittenFromNothingNorErasedByARefusal) {
    Rig rig;
    rig.writer.result = "NOT A CONSULTATION: a cooking video";
    auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});

    ASSERT_TRUE(controller.Open("past"));
    EXPECT_FALSE(controller.RegenerateNote({note::NoteStyle::kProse, note::NoteDetail::kConcise}))
        << "no transcript to write from";
    EXPECT_TRUE(rig.writer.calls.empty());

    rig.store.turns = {
        {0, 16000 * 30, "doctor", "a stored consultation with enough words to note"}};
    ASSERT_TRUE(controller.RegenerateNote({note::NoteStyle::kProse, note::NoteDetail::kConcise}));
    ASSERT_TRUE(rig.events.WaitForNote());
    {
        const std::lock_guard<std::mutex> lock(rig.events.mutex);
        EXPECT_EQ(rig.events.note_refused, "a cooking video");
    }

    controller.Close();
    for (const auto& call : rig.store.Calls()) {
        EXPECT_NE(call, "delete past") << "a refused rewrite never erases the reviewed session";
    }
    EXPECT_TRUE(controller.LastFinalised().empty()) << "leaving still ends the review";
}

}  // namespace
}  // namespace clinicavt::session
