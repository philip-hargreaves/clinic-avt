#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/session/store_failure.hpp"
#include "ports/note_writer.hpp"
#include "ports/session_events.hpp"
#include "ports/session_store.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::session {

// Writes the note, patient sheet and title on a background thread after finalise. One write
// at a time; a new write cancels the running one
class NoteLane {
   public:
    // Below this the model writes from its prompt, not the transcript (seen on a 14 s recording),
    // so the note is refused
    static constexpr std::size_t kMinNoteWords = 25;

    NoteLane(note::INoteWriter* writer, store::ISessionStore& store, ISessionEvents& events,
             std::size_t min_note_words = kMinNoteWords);
    ~NoteLane();
    NoteLane(const NoteLane&) = delete;
    NoteLane& operator=(const NoteLane&) = delete;

    bool Available() const;
    bool WritesPatient() const;
    // True while a document is being written; never blocks
    bool Busy() const;
    // Last note was refused (too short, or not a consultation) and not saved
    bool Refused() const;
    void ClearRefusal();

    // Applied to the next note. The shell sets these ahead of the stop
    void SetOptions(note::NoteOptions options);
    note::NoteOptions Options() const;

    // Writes the note, then sheet and title. Too-short transcripts are refused without calling
    // the model; a model refusal can be overridden. `accepted` runs after the note is stored
    void WriteNote(store::SessionId id, std::vector<asr::Turn> transcript,
                   std::function<void()> accepted = {});
    // Rewrites the sheet from the stored note, edits included, so it matches an edited note
    void WritePatient(store::SessionId id, std::string note);
    // The appraisal case summary from the stored note, edits included
    void WriteSummary(store::SessionId id, std::string note);
    // Cancels a write in progress and waits for its thread
    void Join();

   private:
    void Run(std::function<void()> work);
    void WritePatientNow(const store::SessionId& id, const std::string& note);
    std::optional<store::Document> SaveNote(const store::SessionId& id, const std::string& text,
                                            const note::NoteOptions& options);
    void SaveLabel(const store::SessionId& id, const std::string& note_text);

    note::INoteWriter* writer_;
    store::ISessionStore& store_;
    ISessionEvents& events_;
    std::size_t min_note_words_;
    mutable std::mutex mutex_;  // options_ and thread_
    note::NoteOptions options_;
    std::thread thread_;  // moved out under mutex_, joined outside it
    std::atomic<bool> busy_{false};
    std::atomic<bool> abort_{false};
    std::atomic<bool> refused_{false};
};

}  // namespace clinicavt::session
