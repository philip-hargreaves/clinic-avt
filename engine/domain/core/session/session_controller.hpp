#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/audio/level_meter.hpp"
#include "core/audio/voice_enrolment.hpp"
#include "core/metrics/metrics.hpp"
#include "core/session/capture_source.hpp"
#include "core/session/enrolment.hpp"
#include "core/session/note_lane.hpp"
#include "ports/audio_source.hpp"
#include "ports/diariser.hpp"
#include "ports/note_writer.hpp"
#include "ports/session_events.hpp"
#include "ports/session_store.hpp"
#include "ports/streaming_vad.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::session {

// Import stages and their share of the overall percentage: reading 0-5 %, speech 5-15 %,
// transcribing 15-95 %, finalising 95-100 %
enum class ImportStage { kReading, kSpeech, kTranscribing, kFinalising };

int ImportPercent(ImportStage stage, double fraction);

// Called on the import thread. done: empty error on success, else why the session was erased
struct ImportReport {
    std::function<void(const store::SessionId&, ImportStage stage, int percent)> progress;
    std::function<void(const store::SessionId&, const std::string& error)> done;
};

// Reads a recording, reporting the fraction read
using ImportRead = std::function<std::vector<float>(const std::function<void(double)>& progress)>;

// done() error for an import stopped by Cancel
inline constexpr const char* kImportCancelled = "cancelled";

// One session at a time. Stop finalises, Cancel erases, an interruption leaves it recoverable
class SessionController {
   public:
    // Capture ring size. Covers a first-launch model compile, which stalls the pipeline for a few
    // seconds
    static constexpr std::size_t kCaptureBufferFrames = std::size_t{30} * audio::kSampleRate;

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
    // Evaluation only: stops voice-print updates so held-out runs are reproducible
    void FreezeAnchor();

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
    enum class Outcome { kFinalise, kCancel, kAbandon };

    // Pipeline thread: feeds the store, the diariser's audio buffer and the level meter
    struct PipelineSink : audio::IAudioSink {
        SessionController& controller;

        explicit PipelineSink(SessionController& owner) : controller(owner) {}

        void OnAudio(std::span<const float> frames, std::uint64_t lost_frames) override;
        void OnEnd(const audio::SourceEnd& end) override;
    };

    // Claims the session slot and resets per-session state. False if a session or enrolment runs
    bool Claim();
    // Leaves the previous consultation and begins the stored session. Returns resumed_from's
    // stored audio, or nullopt (claim released) if the store refuses
    std::optional<std::vector<float>> BeginStored(const store::SessionMeta& meta,
                                                  const store::SessionId& resumed_from);
    void GuardedRun();
    void DiarLoop();
    void JoinDiarThread();
    void EndCapture();
    void RunImport(const ImportRead& read, const ImportReport& report);
    // learn=false skips the voice-print update. Returns the actual outcome, which is kCancel
    // for a cancelled import
    Outcome FinishSession(Outcome outcome, bool learn = true);
    std::string StoredNote(const store::SessionId& id) const;

    SourceFactory factory_;
    ISessionEvents& events_;
    store::ISessionStore& store_;
    asr::ITranscriber& transcriber_;
    diar::IDiariser& diariser_;
    note::INoteWriter* note_writer_;
    metrics::Registry* metrics_;
    std::uint64_t diar_advance_frames_;
    std::chrono::milliseconds settle_timeout_;
    NoteLane note_lane_;
    Enrolment enrolment_;
    std::unique_ptr<audio::IAudioSource> source_;
    std::thread worker_;
    std::thread diar_thread_;
    // Started and joined on the RPC thread
    std::thread import_thread_;
    bool importing_ = false;  // under mutex_
    std::atomic<bool> import_cancel_{false};
    // Set on the import thread for its finalise
    std::function<void(ImportStage, double fraction)> import_progress_;
    bool diar_stop_ = false;  // under mutex_
    int diar_ticks_ = 0;      // under mutex_, diagnostics
    audio::LevelMeter meter_;
    bool learn_anchor_ = true;  // set before Start
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    bool got_audio_ = false;
    bool ended_ = false;
    bool stop_requested_ = false;
    std::uint64_t lost_frames_ = 0;
    store::SessionId session_id_;
    store::SessionId resumed_from_;
    bool note_prepared_ = false;  // diar thread only
    store::SessionId last_finalised_;
    bool reviewing_ = false;  // last_finalised_ came from Open
    // Appended under mutex_ (the diarisation thread snapshots it). Finalise
    // reads it after every other thread has joined
    std::vector<float> session_audio_;
    audio::SourceEnd end_{};
};

}  // namespace clinicavt::session
