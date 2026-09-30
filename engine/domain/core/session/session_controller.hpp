#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/audio/voice_enrolment.hpp"
#include "core/metrics/metrics.hpp"
#include "core/session/capture_lane.hpp"
#include "core/session/capture_source.hpp"
#include "core/session/enrolment.hpp"
#include "core/session/finaliser.hpp"
#include "core/session/import_job.hpp"
#include "core/session/import_progress.hpp"
#include "core/session/note_lane.hpp"
#include "core/session/review.hpp"
#include "core/session/session_state.hpp"
#include "ports/audio_source.hpp"
#include "ports/diariser.hpp"
#include "ports/note_writer.hpp"
#include "ports/session_events.hpp"
#include "ports/session_store.hpp"
#include "ports/streaming_vad.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::session {

// One session at a time. Stop finalises, Cancel erases, an interruption leaves it recoverable
class SessionController {
   public:
    SessionController(SourceFactory factory, ISessionEvents& events, store::ISessionStore& store,
                      asr::ITranscriber& transcriber, audio::IStreamingVad& vad,
                      diar::IDiariser& diariser,
                      std::chrono::milliseconds settle_timeout = std::chrono::seconds(3),
                      std::uint64_t diar_advance_frames = std::uint64_t{5} * audio::kSampleRate,
                      note::INoteWriter* note_writer = nullptr,
                      metrics::Registry* metrics = nullptr,
                      std::size_t min_note_words = NoteLane::kMinNoteWords);
    // Joins all threads before members are destroyed
    ~SessionController();
    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;

    // True once audio flows. resume_from replays stored audio before live input; retain=false
    // erases it once the consultation is left
    bool Start(std::optional<ReplaySpec> replay = std::nullopt,
               const store::SessionId& resume_from = {}, bool retain = true,
               const MicSelection& mic = {});
    // Imports an external recording dated started_at (empty: now). Returns the new session id and
    // finalises on the import thread like Stop, without updating the voice print. Empty if a
    // session or enrolment is running or the store refuses
    store::SessionId Import(ImportRead read, const std::string& started_at, bool retain,
                            ImportReport report);
    bool Importing() const;
    // Idempotent. Keeps the recording and never counts as an interruption. A running import
    // completes first
    void Stop();
    // Idempotent. The recording is erased. An import stops at its next span and is erased
    // on its own thread
    void Cancel();
    bool Running() const;
    // Capturing, or still writing the consultation's note, sheet or case summary
    bool Busy() const;

    // Enrolment is refused while a consultation runs, and recording while an
    // enrolment runs. The lock orders the two checks
    bool StartEnrolment(double seconds, const MicSelection& mic = {},
                        double min_speech_s = audio::kEnrolMinSpeechSeconds);
    void CancelEnrolment();
    void FinishEnrolment();

    audio::SourceEnd LastEnd() const;
    // The most recently finalised session, for the shell's post-stop
    // transcript fetch, empty until a session has finalised
    store::SessionId LastFinalised() const;
    // Reopens a stored session as the regenerate target, refused while
    // recording or writing
    bool Open(const store::SessionId& id);
    // Ends a review (regenerate refused until the next finalise or Open), deletes a just-recorded
    // session whose note was refused (never a reviewed one), and erases it if retain was off
    void Close();
    // The recording session's id, so the shell can resume it after a crash
    store::SessionId CurrentSession() const;

    // Applied to the next note. The shell sets these ahead of the stop
    void SetNoteOptions(note::NoteOptions options);
    bool HasNoteWriter() const;
    // Rewrites the last finalised session's note. False when busy, so the RPC
    // thread never blocks on the lane, or when the stored transcript has no turns
    bool RegenerateNote(note::NoteOptions options);
    // Case summary from the stored note, edits included, for any stored
    // session. False when busy or without a note
    bool WriteSummary(store::SessionId id);
    // Rewrites the patient sheet from the stored note, edits included
    bool RegeneratePatient();

   private:
    // Claims the session slot and resets per-session state. False if a session or enrolment runs
    bool Claim();
    // Leaves the previous consultation and begins the stored session. Returns resumed_from's
    // stored audio, or nullopt (claim released) if the store refuses
    std::optional<std::vector<float>> BeginStored(const store::SessionMeta& meta,
                                                  const store::SessionId& resumed_from);

    SourceFactory factory_;
    ISessionEvents& events_;
    store::ISessionStore& store_;
    note::INoteWriter* note_writer_;
    metrics::Registry* metrics_;
    std::chrono::milliseconds settle_timeout_;
    SessionState state_;
    NoteLane note_lane_;
    Enrolment enrolment_;
    CaptureLane capture_;
    Finaliser finaliser_;
    ImportJob import_;
    Review review_;
};

}  // namespace clinicavt::session
