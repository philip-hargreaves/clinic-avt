#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
#include "core/session/enrolment.hpp"
#include "core/session/note_lane.hpp"
#include "core/session/session_events.hpp"
#include "ports/audio_source.hpp"
#include "ports/diariser.hpp"
#include "ports/note_writer.hpp"
#include "ports/session_store.hpp"
#include "ports/streaming_vad.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::session {

// One session at a time. Every ending has a storage outcome: Stop
// finalises, Cancel erases, an interruption abandons recoverable
class SessionController {
   public:
    // Audio the capture thread can run ahead of the pipeline before frames are
    // lost. A first-launch model compile stalls for a few seconds
    static constexpr std::size_t kCaptureBufferFrames = 30 * audio::kSampleRate;

    SessionController(SourceFactory factory, ISessionEvents& events, store::ISessionStore& store,
                      asr::ITranscriber& transcriber, audio::IStreamingVad& vad,
                      diar::IDiariser& diariser,
                      std::chrono::milliseconds settle_timeout = std::chrono::seconds(3),
                      std::uint64_t diar_advance_frames = 5 * audio::kSampleRate,
                      note::INoteWriter* note_writer = nullptr,
                      metrics::Registry* metrics = nullptr,
                      std::size_t min_note_words = NoteLane::kMinNoteWords);
    // Every lane is joined before the members they read are destroyed
    ~SessionController();
    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;

    // True once audio flows. resume replays stored audio ahead of the live
    // source, and retain false erases once the consultation is left
    bool Start(std::optional<ReplaySpec> replay = std::nullopt,
               const store::SessionId& resume_from = {}, bool retain = true,
               const MicSelection& mic = {});
    // Idempotent. A stop is the user's, so it never counts as an interruption.
    // The recording is kept
    void Stop();
    // Idempotent. The recording is erased
    void Cancel();
    // Holds the source's delivery. Stop and cancel always win
    void SetPaused(bool paused);
    void SetMonitor(bool monitor);
    bool Running() const;
    // Capturing, or still writing the consultation's note, sheet or case summary. A note model
    // switch or a delete is refused meanwhile
    bool Busy() const;
    // Evaluation only: the print never learns, so a held-out run is reproducible
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
    // Leaving the consultation: ends a review (regenerate refuses until the
    // next finalise or open), deletes a just-recorded session that ended in a
    // refusal (never a reviewed one), and erases what was recorded with retain off
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
    // The sheet rewritten from the stored note, clinician edits included
    bool RegeneratePatient();

   private:
    enum class Outcome { kFinalise, kCancel, kAbandon };

    // The pipeline thread: the store, the diariser's audio, the meter
    struct PipelineSink : audio::IAudioSink {
        SessionController& controller;

        explicit PipelineSink(SessionController& owner) : controller(owner) {}

        void OnAudio(std::span<const float> frames, std::uint64_t lost_frames) override;
        void OnEnd(const audio::SourceEnd& end) override;
    };

    void GuardedRun();
    void DiarLoop();
    void JoinDiarThread();
    void EndCapture();
    void FinishSession(Outcome outcome);
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
