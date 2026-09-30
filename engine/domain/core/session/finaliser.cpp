#include "core/session/finaliser.hpp"

#include <exception>
#include <mutex>
#include <span>
#include <string>
#include <utility>

#include "core/common/log.hpp"
#include "core/session/store_failure.hpp"
#include "core/session/transcribe_recording.hpp"

namespace clinicavt::session {

Finaliser::StageClock::StageClock(metrics::Registry* metrics) : metrics_(metrics) {}

void Finaliser::StageClock::operator()(const char* name) const {
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    log::Printf("clinicavt-engine: finalise %s at %.1f s\n", name, seconds);
    if (metrics_ != nullptr) {
        metrics_->RecordStage(name, seconds);
    }
}

Finaliser::Finaliser(SessionState& state, CaptureLane& capture, NoteLane& note_lane,
                     ICaptureEvents& events, store::IRecordingStore& store,
                     store::ISessionCatalog& catalog, asr::ITranscriber& transcriber,
                     diar::IDiariser& diariser, note::INoteWriter* note_writer,
                     metrics::Registry* metrics)
    : state_(state),
      capture_(capture),
      note_lane_(note_lane),
      events_(events),
      store_(store),
      catalog_(catalog),
      transcriber_(transcriber),
      diariser_(diariser),
      note_writer_(note_writer),
      metrics_(metrics) {}

Outcome Finaliser::Finish(Outcome outcome, bool learn, const ImportHooks* import) {
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        if (state_.session_id.empty()) {
            return outcome;
        }
    }
    const StageClock stage(metrics_);
    JoinCapture(stage);
    if (outcome == Outcome::kFinalise) {
        outcome = SettleCapture(outcome, import, stage);
    }
    const store::SessionId id = TakeSession(outcome);
    if (id.empty()) {
        return outcome;
    }
    Sealed sealed;
    if (outcome == Outcome::kFinalise && !state_.session_audio.empty()) {
        sealed = SealTranscript(id, learn, import, stage);
    }
    ClearCapture();
    outcome = HonourLateCancel(outcome, id, import);
    StoreOutcome(id, outcome, stage);
    DeleteResumedFrom();
    if (outcome == Outcome::kFinalise && note_writer_ != nullptr) {
        StartNote(id, std::move(sealed), stage);
    }
    return outcome;
}

// No capture work may run during finalise
void Finaliser::JoinCapture(const StageClock& stage) {
    capture_.JoinDiarisation();
    stage("capture joined");
    log::Printf("clinicavt-engine: session audio %.1f s, %d capture ticks\n",
                static_cast<double>(state_.session_audio.size()) / audio::kSampleRate,
                state_.diar_ticks);
    if (metrics_ != nullptr) {
        metrics_->RecordSession(
            static_cast<double>(state_.session_audio.size()) / audio::kSampleRate,
            state_.lost_frames, state_.diar_ticks);
    }
}

// Capture can lag, so the remaining spans decode now and every cut reaches the diariser
Outcome Finaliser::SettleCapture(Outcome outcome, const ImportHooks* import,
                                 const StageClock& stage) {
    const bool reporting = import != nullptr && import->progress;
    const auto cancelled = [import] { return import != nullptr && import->cancel.load(); };
    events_.OnProgress("transcript");
    try {
        auto* const capture = diariser_.Capture();
        // Imports run VAD as a separate pass to report progress. Live capture has already run it
        if (reporting && capture != nullptr) {
            capture->FindSpeech(
                state_.session_audio,
                [import](double fraction) { import->progress(ImportStage::kSpeech, fraction); },
                cancelled);
        }
        // Spans decode in order, so each span's end gives the import's progress
        const double total = static_cast<double>(state_.session_audio.size());
        if (capture != nullptr) {
            capture->Settle(
                state_.session_audio,
                [this, import, reporting, total, &cancelled](std::span<const float> clip,
                                                             std::uint64_t first) {
                    auto chunks = transcriber_.DecodeClipChunks(clip, first, cancelled);
                    if (reporting) {
                        import->progress(ImportStage::kTranscribing,
                                         static_cast<double>(first + clip.size()) / total);
                    }
                    return chunks;
                },
                cancelled);
        }
        const auto cuts = transcriber_.TakeClipCuts();
        if (!cuts.empty() && capture != nullptr) capture->AddCutPoints(cuts);
    } catch (const std::exception& e) {
        log::Printf("clinicavt-engine: capture settle failed: %s\n", e.what());
    } catch (...) {
        log::Printf("clinicavt-engine: capture settle failed\n");
    }
    stage("capture settled");
    if (cancelled()) {
        return Outcome::kCancel;
    }
    if (reporting) {
        import->progress(ImportStage::kFinalising, 0.0);
    }
    return outcome;
}

store::SessionId Finaliser::TakeSession(Outcome outcome) {
    std::lock_guard<std::mutex> lock(state_.mutex);
    store::SessionId id = std::exchange(state_.session_id, {});
    if (outcome == Outcome::kFinalise) {
        state_.last_finalised = id;
        note_lane_.ClearRefusal();
    }
    return id;
}

// note_input stays empty if diarisation fails. The note is then refused as too short and the
// session is kept
Finaliser::Sealed Finaliser::SealTranscript(const store::SessionId& id, bool learn,
                                            const ImportHooks* import, const StageClock& stage) {
    Sealed sealed;
    try {
        auto transcript = TranscribeRecording(state_.session_audio, diariser_, transcriber_,
                                              events_, metrics_, stage);
        if (!transcript.turns.empty()) {
            store_.ReplaceTurns(id, transcript.turns);
            sealed.note_input = std::move(transcript.turns);
        }
        stage("transcript sealed");
        if (import != nullptr && import->progress) {
            import->progress(ImportStage::kFinalising, 1.0);
        }
        // Update the voice print only when a doctor was named and, with a note writer, only
        // after the note is accepted
        auto* const voiceprints = diariser_.Voiceprints();
        if (transcript.doctor_cluster >= 0 && learn && voiceprints != nullptr) {
            sealed.doctor_voiceprint = voiceprints->DoctorVoiceprint(
                state_.session_audio, transcript.diarised.slices, transcript.doctor_cluster);
            if (note_writer_ == nullptr) {
                voiceprints->AccrueVoiceprint(sealed.doctor_voiceprint);
                sealed.doctor_voiceprint.clear();
            }
        }
        stage("anchor accrued");
    } catch (const std::exception& e) {
        log::Printf("clinicavt-engine: transcription failed: %s\n", e.what());
    } catch (...) {
        log::Printf("clinicavt-engine: transcription failed\n");
    }
    return sealed;
}

// Leftover capture state and cuts must not leak into the next session
void Finaliser::ClearCapture() {
    if (auto* capture = diariser_.Capture()) capture->DiscardCapture();
    (void)transcriber_.TakeClipCuts();
    state_.session_audio.clear();
    state_.session_audio.shrink_to_fit();
}

// A cancel can arrive during transcription
Outcome Finaliser::HonourLateCancel(Outcome outcome, const store::SessionId& id,
                                    const ImportHooks* import) {
    if (outcome != Outcome::kFinalise || import == nullptr || !import->cancel.load()) {
        return outcome;
    }
    std::lock_guard<std::mutex> lock(state_.mutex);
    if (state_.last_finalised == id) state_.last_finalised.clear();
    return Outcome::kCancel;
}

void Finaliser::StoreOutcome(const store::SessionId& id, Outcome outcome, const StageClock& stage) {
    try {
        switch (outcome) {
            case Outcome::kFinalise:
                store_.Finalise(id);
                break;
            case Outcome::kCancel:
                store_.Cancel(id);
                break;
            case Outcome::kAbandon:
                store_.Abandon(id);
                break;
        }
    } catch (const std::exception& e) {
        ReportStoreFailure(events_, "outcome", e);
    }
    stage("stored");
}

// The resumed-from session's audio was replayed into this one, so it is deleted
void Finaliser::DeleteResumedFrom() {
    std::string resumed;
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        resumed = std::exchange(state_.resumed_from, {});
    }
    if (!resumed.empty()) {
        try {
            catalog_.Delete(resumed);
        } catch (...) {  // NOLINT(bugprone-empty-catch) the session stays, as if kept
        }
    }
}

void Finaliser::StartNote(const store::SessionId& id, Sealed sealed, const StageClock& stage) {
    stage("note lane started");
    note_lane_.WriteNote(id, std::move(sealed.note_input),
                         [this, print = std::move(sealed.doctor_voiceprint)] {
                             if (print.empty()) return;
                             if (auto* voiceprints = diariser_.Voiceprints()) {
                                 voiceprints->AccrueVoiceprint(print);
                             }
                         });
}

}  // namespace clinicavt::session
