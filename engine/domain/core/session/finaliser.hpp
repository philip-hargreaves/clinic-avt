#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <vector>

#include "core/metrics/metrics.hpp"
#include "core/session/capture_lane.hpp"
#include "core/session/import_progress.hpp"
#include "core/session/note_lane.hpp"
#include "core/session/session_state.hpp"
#include "ports/diariser.hpp"
#include "ports/note_writer.hpp"
#include "ports/session_events.hpp"
#include "ports/session_store.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::session {

enum class Outcome { kFinalise, kCancel, kAbandon };

// An import's cancel flag and progress, read during its finalise
struct ImportHooks {
    const std::atomic<bool>& cancel;
    std::function<void(ImportStage, double fraction)> progress;
};

// Ends a session for Stop, import, Cancel and abandon
class Finaliser {
   public:
    Finaliser(SessionState& state, CaptureLane& capture, NoteLane& note_lane,
              ISessionEvents& events, store::ISessionStore& store, asr::ITranscriber& transcriber,
              diar::IDiariser& diariser, note::INoteWriter* note_writer,
              metrics::Registry* metrics);

    // learn=false skips the voice-print update. Returns the actual outcome, which is kCancel
    // for a cancelled import. The store outcome is applied even if the steps before it fail
    Outcome Finish(Outcome outcome, bool learn = true, const ImportHooks* import = nullptr);

   private:
    // Logs each stage's time since the finalise began, to locate slow stages
    class StageClock {
       public:
        explicit StageClock(metrics::Registry* metrics);
        void operator()(const char* name) const;

       private:
        metrics::Registry* metrics_;
        std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    };

    struct Sealed {
        std::vector<asr::Turn> note_input;
        std::vector<float> doctor_voiceprint;
    };

    void JoinCapture(const StageClock& stage);
    Outcome SettleCapture(Outcome outcome, const ImportHooks* import, const StageClock& stage);
    store::SessionId TakeSession(Outcome outcome);
    Sealed SealTranscript(const store::SessionId& id, bool learn, const ImportHooks* import,
                          const StageClock& stage);
    void ClearCapture();
    Outcome HonourLateCancel(Outcome outcome, const store::SessionId& id,
                             const ImportHooks* import);
    void StoreOutcome(const store::SessionId& id, Outcome outcome, const StageClock& stage);
    void DeleteResumedFrom();
    void StartNote(const store::SessionId& id, Sealed sealed, const StageClock& stage);

    SessionState& state_;
    CaptureLane& capture_;
    NoteLane& note_lane_;
    ISessionEvents& events_;
    store::ISessionStore& store_;
    asr::ITranscriber& transcriber_;
    diar::IDiariser& diariser_;
    note::INoteWriter* note_writer_;
    metrics::Registry* metrics_;
};

}  // namespace clinicavt::session
