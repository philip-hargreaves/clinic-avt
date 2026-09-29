#include "core/session/session_controller.hpp"

#include <algorithm>
#include <exception>
#include <utility>

#include "core/audio/buffered_sink.hpp"
#include "core/audio/resume_source.hpp"
#include "core/audio/voice_enrolment.hpp"
#include "core/common/log.hpp"
#include "core/diarisation/tidy_transcript.hpp"
#include "core/session/store_failure.hpp"
#include "core/session/transcribe_recording.hpp"

namespace clinicavt::session {

SessionController::SessionController(SourceFactory factory, ISessionEvents& events,
                                     store::ISessionStore& store, asr::ITranscriber& transcriber,
                                     audio::IStreamingVad& vad, diar::IDiariser& diariser,
                                     std::chrono::milliseconds settle_timeout,
                                     std::uint64_t diar_advance_frames,
                                     note::INoteWriter* note_writer, metrics::Registry* metrics,
                                     std::size_t min_note_words)
    : factory_(std::move(factory)),
      events_(events),
      store_(store),
      transcriber_(transcriber),
      diariser_(diariser),
      note_writer_(note_writer),
      metrics_(metrics),
      diar_advance_frames_(diar_advance_frames),
      settle_timeout_(settle_timeout),
      note_lane_(note_writer, store, events, min_note_words),
      enrolment_(factory_, vad, diariser, events) {
    store_.SetFaultListener(
        [this](const store::StoreError& fault) { events_.OnStorageFault(fault.what()); });
}

SessionController::~SessionController() {
    Stop();
    enrolment_.Cancel();
    enrolment_.Join();
    note_lane_.Join();
}

bool SessionController::Start(std::optional<ReplaySpec> replay, const store::SessionId& resume_from,
                              bool retain, const MicSelection& mic) {
    if (!Claim()) {
        return false;
    }
    store::SessionMeta meta{audio::kSampleRate, "", ""};
    if (!replay.has_value()) {
        // Record the device actually opened, which may be a default fallback
        meta.device_id = mic.id;
        meta.device_name = mic.name;
    }
    meta.retain = retain;
    auto stored = BeginStored(meta, resume_from);
    if (!stored.has_value()) {
        return false;
    }
    std::vector<float> resumed_audio = std::move(*stored);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!resumed_audio.empty()) {
            if (replay.has_value()) {
                replay->start_frame += resumed_audio.size();
            }
            source_ = std::make_unique<audio::ResumeSource>(std::move(resumed_audio),
                                                            factory_(replay, mic.id));
        } else {
            source_ = factory_(replay, mic.id);
        }
    }
    worker_ = std::thread([this] { GuardedRun(); });
    diar_thread_ = std::thread([this] { DiarLoop(); });
    if (metrics_ != nullptr) {
        metrics_->BeginSession(replay.has_value(), replay.has_value() ? replay->speed : 0.0);
    }

    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, settle_timeout_, [this] { return got_audio_ || ended_; });
    if (got_audio_) {
        return true;
    }
    lock.unlock();
    Cancel();  // no audio arrived, so erase the session

    std::lock_guard<std::mutex> relock(mutex_);
    if (end_.reason == audio::SourceEndReason::kStopped) {
        end_ = {audio::SourceEndReason::kFailed, "no audio arrived before the deadline"};
    }
    return false;
}

int ImportPercent(ImportStage stage, double fraction) {
    struct Band {
        int from;
        int to;
    };
    static constexpr Band kBands[] = {{0, 5}, {5, 15}, {15, 95}, {95, 100}};
    const Band band = kBands[static_cast<int>(stage)];
    const double within = std::clamp(fraction, 0.0, 1.0) * (band.to - band.from);
    return band.from + static_cast<int>(within);
}

store::SessionId SessionController::Import(ImportRead read, const std::string& started_at,
                                           bool retain, ImportReport report) {
    if (Importing()) {
        return {};
    }
    if (import_thread_.joinable()) {
        import_thread_.join();  // the previous import, already finished
    }
    if (!Claim()) {
        return {};
    }
    store::SessionMeta meta{audio::kSampleRate, "", ""};
    meta.retain = retain;
    meta.started_at = started_at;
    if (!BeginStored(meta, {}).has_value()) {
        return {};
    }
    store::SessionId id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = session_id_;
        importing_ = true;
        source_.reset();  // stop the last recording's source feeding the import
    }
    import_cancel_ = false;
    import_thread_ = std::thread(
        [this, read = std::move(read), report = std::move(report)] { RunImport(read, report); });
    return id;
}

bool SessionController::Importing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return importing_;
}

// Catches everything. Frees the session slot before done, so a request made from done isn't
// refused as busy
void SessionController::RunImport(const ImportRead& read, const ImportReport& report) {
    const store::SessionId id = CurrentSession();
    const auto progress = [&report, &id](ImportStage stage, double fraction) {
        if (report.progress) report.progress(id, stage, ImportPercent(stage, fraction));
    };
    std::string error;
    try {
        std::vector<float> recording =
            read([&progress](double fraction) { progress(ImportStage::kReading, fraction); });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_audio_ = std::move(recording);
        }
        if (metrics_ != nullptr) {
            metrics_->BeginSession(true, 0.0);
        }
        if (note_writer_ != nullptr) {
            note_writer_->Prepare();
        }
        import_progress_ = progress;
        // Imported audio may be another clinician, so don't update the voice print
        if (import_cancel_ || FinishSession(Outcome::kFinalise, false) == Outcome::kCancel) {
            error = kImportCancelled;
        }
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "the recording could not be imported";
    }
    import_progress_ = nullptr;
    if (!error.empty()) {
        // Erase the session begun before the failed read
        try {
            FinishSession(Outcome::kCancel);
        } catch (...) {  // NOLINT(bugprone-empty-catch) the import error is reported
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        importing_ = false;
        import_cancel_ = false;
    }
    if (report.done) {
        try {
            report.done(id, error);
        } catch (...) {  // NOLINT(bugprone-empty-catch) the shell may already be gone
        }
    }
}

bool SessionController::Claim() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_ || enrolment_.Running()) {
        return false;
    }
    running_ = true;
    reviewing_ = false;  // a new session ends any review
    got_audio_ = false;
    ended_ = false;
    stop_requested_ = false;
    diar_stop_ = false;
    diar_ticks_ = 0;
    lost_frames_ = 0;
    end_ = {};
    meter_ = audio::LevelMeter{};
    return true;
}

std::optional<std::vector<float>> SessionController::BeginStored(
    const store::SessionMeta& meta, const store::SessionId& resumed_from) {
    try {
        std::vector<float> resumed_audio;
        if (!resumed_from.empty()) {
            resumed_audio = store_.ReadAudio(resumed_from);
            log::Printf("clinicavt-engine: resuming %s with %.1f s of stored audio\n",
                        resumed_from.c_str(),
                        static_cast<double>(resumed_audio.size()) / audio::kSampleRate);
        }
        store_.EraseUnretained();  // erase the previous consultation unless it was retained
        const store::SessionId id = store_.Begin(meta);
        std::lock_guard<std::mutex> lock(mutex_);
        session_id_ = id;
        resumed_from_ = resumed_from;
        note_prepared_ = false;
        session_audio_.clear();
        return resumed_audio;
    } catch (const std::exception& e) {
        log::Printf("clinicavt-engine: session start failed: %s\n", e.what());
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        end_ = {audio::SourceEndReason::kFailed, std::string("session setup failed: ") + e.what()};
        return std::nullopt;
    }
}

void SessionController::Stop() {
    if (import_thread_.joinable()) {
        import_thread_.join();
    }
    // Re-warm the note model during finalise; a long session may have evicted it
    if (note_writer_ != nullptr && Running()) {
        note_writer_->Prepare();
    }
    EndCapture();
    FinishSession(Outcome::kFinalise);
}

void SessionController::Cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (importing_) {
            import_cancel_ = true;
            return;
        }
    }
    EndCapture();
    FinishSession(Outcome::kCancel);
}

bool SessionController::Running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && !ended_;
}

bool SessionController::Busy() const {
    return Running() || note_lane_.Busy();
}

void SessionController::FreezeAnchor() {
    learn_anchor_ = false;
}

bool SessionController::StartEnrolment(double seconds, const MicSelection& mic,
                                       double min_speech_s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return false;
    return enrolment_.Start(seconds, mic, min_speech_s);
}

void SessionController::CancelEnrolment() {
    enrolment_.Cancel();
}

void SessionController::FinishEnrolment() {
    enrolment_.Finish();
}

audio::SourceEnd SessionController::LastEnd() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return end_;
}

store::SessionId SessionController::LastFinalised() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_finalised_;
}

bool SessionController::Open(const store::SessionId& id) {
    if (Running() || note_lane_.Busy()) {
        return false;
    }
    try {
        (void)store_.ReadTurns(id);
    } catch (...) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    last_finalised_ = id;
    reviewing_ = true;
    note_lane_.ClearRefusal();
    return true;
}

void SessionController::Close() {
    store::SessionId refused;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (note_lane_.Refused()) {
            // Delete only a fresh capture whose note was refused. A reviewed session is kept
            if (!reviewing_) refused = std::exchange(last_finalised_, {});
            note_lane_.ClearRefusal();
        }
        if (reviewing_) {
            reviewing_ = false;
            last_finalised_.clear();
        }
    }
    if (!refused.empty()) {
        try {
            store_.Delete(refused);
        } catch (...) {  // NOLINT(bugprone-empty-catch) the session stays, as if kept
        }
    }
    if (!Running()) {
        try {
            store_.EraseUnretained();
        } catch (...) {  // NOLINT(bugprone-empty-catch) retried at the next start
        }
    }
}

store::SessionId SessionController::CurrentSession() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_id_;
}

void SessionController::SetNoteOptions(note::NoteOptions options) {
    note_lane_.SetOptions(options);
}

bool SessionController::HasNoteWriter() const {
    return note_lane_.Available();
}

bool SessionController::RegenerateNote(note::NoteOptions options) {
    if (!note_lane_.Available() || Running() || note_lane_.Busy()) {
        return false;
    }
    store::SessionId id = LastFinalised();
    if (id.empty()) {
        return false;
    }
    std::vector<asr::Turn> turns;
    try {
        turns = store_.ReadTurns(id);
    } catch (...) {
        return false;
    }
    // A session restored without its transcript has nothing to write from
    if (turns.empty()) {
        return false;
    }
    note_lane_.SetOptions(options);
    note_lane_.WriteNote(std::move(id), std::move(turns));
    return true;
}

bool SessionController::WriteSummary(store::SessionId id) {
    if (!note_lane_.Available() || Running() || note_lane_.Busy()) {
        return false;
    }
    std::string note = StoredNote(id);
    if (note.empty()) {
        return false;
    }
    note_lane_.WriteSummary(std::move(id), std::move(note));
    return true;
}

bool SessionController::RegeneratePatient() {
    if (!note_lane_.WritesPatient() || Running() || note_lane_.Busy()) {
        return false;
    }
    store::SessionId id = LastFinalised();
    if (id.empty()) {
        return false;
    }
    std::string note = StoredNote(id);
    if (note.empty()) {
        return false;
    }
    note_lane_.WritePatient(std::move(id), std::move(note));
    return true;
}

void SessionController::PipelineSink::OnAudio(std::span<const float> frames,
                                              std::uint64_t lost_frames) {
    store::SessionId id;
    {
        std::lock_guard<std::mutex> lock(controller.mutex_);
        controller.lost_frames_ += lost_frames;
        controller.got_audio_ = true;
        id = controller.session_id_;
        // Under the lock: the diarisation thread snapshots this
        if (!id.empty()) {
            controller.session_audio_.insert(controller.session_audio_.end(), frames.begin(),
                                             frames.end());
        }
    }
    controller.cv_.notify_all();
    if (!id.empty()) {
        controller.store_.Append(id, frames, lost_frames);
    }
    for (const auto& reading : controller.meter_.Push(frames)) {
        controller.events_.OnLevel(reading);
    }
}

void SessionController::PipelineSink::OnEnd(const audio::SourceEnd& end) {
    bool interrupted = false;
    {
        std::lock_guard<std::mutex> lock(controller.mutex_);
        controller.end_ = end;
        controller.ended_ = true;
        interrupted = controller.got_audio_ && !controller.stop_requested_ &&
                      (end.reason == audio::SourceEndReason::kDeviceLost ||
                       end.reason == audio::SourceEndReason::kFailed);
    }
    controller.cv_.notify_all();
    // Only interruptions finish here; otherwise Stop or Cancel decides
    if (interrupted) {
        controller.FinishSession(Outcome::kAbandon);
        controller.events_.OnInterrupted(end.reason, end.detail);
    }
}

// Catches everything, since an exception leaving a thread function calls std::terminate
void SessionController::GuardedRun() {
    PipelineSink sink(*this);
    audio::BufferedSink buffered(sink, kCaptureBufferFrames);
    try {
        source_->Run(buffered);
    } catch (const std::exception& e) {
        buffered.OnEnd(
            {audio::SourceEndReason::kFailed, std::string("capture thread threw: ") + e.what()});
    } catch (...) {
        buffered.OnEnd({audio::SourceEndReason::kFailed, "capture thread threw"});
    }
}

// Incremental diarisation during capture. Advance runs outside the lock, off the pipeline
// thread
void SessionController::DiarLoop() {
    // Minimum wall-clock gap between ticks, for faster-than-real-time replay
    constexpr auto kMinTickGap = std::chrono::seconds(1);
    std::vector<float> audio;
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait(lock, [this, &audio] {
            return diar_stop_ || session_audio_.size() >= audio.size() + diar_advance_frames_;
        });
        if (diar_stop_) {
            return;
        }
        audio = session_audio_;
        ++diar_ticks_;
        lock.unlock();
        // Deferred until whisper is decoding, so the GPU never compiles two models at once
        if (!note_prepared_ && note_writer_ != nullptr) {
            note_prepared_ = true;
            note_writer_->Prepare();
        }
        try {
            const auto decode = [this](std::span<const float> clip,
                                       std::uint64_t first) -> std::vector<asr::Turn> {
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    // Skip speculative decodes once stopping
                    if (diar_stop_) return {};
                }
                return transcriber_.DecodeClipChunks(clip, first);
            };
            diariser_.Advance(audio, decode);
            // Apply this tick's chunk-edge cuts and decode the pieces now, so Stop doesn't have to
            const auto cuts = transcriber_.TakeClipCuts();
            if (!cuts.empty()) {
                diariser_.AddCutPoints(cuts);
                diariser_.Advance(audio, decode);
            }
            // Prefill the note model's KV cache with the settled transcript, tidied as finalise
            // does so the prefix matches
            if (note_writer_ != nullptr) {
                auto guess = diar::TidyTranscript(diariser_.SpeculativeTranscript());
                if (!guess.empty()) note_writer_->Prefill(guess, note_lane_.Options());
            }
        } catch (const std::exception& e) {
            log::Printf("clinicavt-engine: capture tick failed: %s\n", e.what());
        } catch (...) {
            log::Printf("clinicavt-engine: capture tick failed\n");
        }
        lock.lock();
        cv_.wait_for(lock, kMinTickGap, [this] { return diar_stop_; });
        if (diar_stop_) {
            return;
        }
    }
}

void SessionController::JoinDiarThread() {
    std::thread diar;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        diar_stop_ = true;
        diar = std::move(diar_thread_);
    }
    cv_.notify_all();
    if (diar.joinable()) {
        diar.join();
    }
}

void SessionController::EndCapture() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }
    if (source_) {
        source_->RequestStop();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
}

// Common end for Stop, import, Cancel and abandon. The store outcome is applied even if the
// steps before it fail
SessionController::Outcome SessionController::FinishSession(Outcome outcome, bool learn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_id_.empty()) {
            return outcome;
        }
    }
    // Join capture first; no capture work may run during finalise. Stage timings locate slow stages
    const auto finalise_start = std::chrono::steady_clock::now();
    const auto stage = [this, &finalise_start](const char* name) {
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - finalise_start)
                .count();
        log::Printf("clinicavt-engine: finalise %s at %.1f s\n", name, seconds);
        if (metrics_ != nullptr) {
            metrics_->RecordStage(name, seconds);
        }
    };
    JoinDiarThread();
    stage("capture joined");
    log::Printf("clinicavt-engine: session audio %.1f s, %d capture ticks\n",
                static_cast<double>(session_audio_.size()) / audio::kSampleRate, diar_ticks_);
    if (metrics_ != nullptr) {
        metrics_->RecordSession(static_cast<double>(session_audio_.size()) / audio::kSampleRate,
                                lost_frames_, diar_ticks_);
    }
    // Capture can lag; decode the remaining spans now so every cut reaches the diariser
    if (outcome == Outcome::kFinalise) {
        events_.OnProgress("transcript");
        try {
            const auto stop = [this] { return import_cancel_.load(); };
            // Imports run VAD as a separate pass to report progress; live capture already has it
            if (import_progress_) {
                diariser_.FindSpeech(
                    session_audio_,
                    [this](double fraction) { import_progress_(ImportStage::kSpeech, fraction); },
                    stop);
            }
            // Spans decode in order, so each one's end is the import's progress
            const double total = static_cast<double>(session_audio_.size());
            diariser_.Settle(
                session_audio_,
                [this, total](std::span<const float> clip, std::uint64_t first) {
                    auto chunks = transcriber_.DecodeClipChunks(clip, first);
                    if (import_progress_) {
                        import_progress_(ImportStage::kTranscribing,
                                         static_cast<double>(first + clip.size()) / total);
                    }
                    return chunks;
                },
                stop);
            const auto cuts = transcriber_.TakeClipCuts();
            if (!cuts.empty()) diariser_.AddCutPoints(cuts);
        } catch (const std::exception& e) {
            log::Printf("clinicavt-engine: capture settle failed: %s\n", e.what());
        } catch (...) {
            log::Printf("clinicavt-engine: capture settle failed\n");
        }
        stage("capture settled");
        if (import_cancel_) {
            outcome = Outcome::kCancel;
        } else if (import_progress_) {
            import_progress_(ImportStage::kFinalising, 0.0);
        }
    }

    store::SessionId id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = std::exchange(session_id_, {});
        if (outcome == Outcome::kFinalise) {
            last_finalised_ = id;
            note_lane_.ClearRefusal();
        }
    }
    if (id.empty()) {
        return outcome;
    }
    // Empty if diarisation fails; the note is then refused as too short but the session is kept
    std::vector<asr::Turn> note_input;
    std::vector<float> doctor_voiceprint;
    if (outcome == Outcome::kFinalise && !session_audio_.empty()) {
        try {
            auto transcript = TranscribeRecording(session_audio_, diariser_, transcriber_, events_,
                                                  metrics_, stage);
            if (!transcript.turns.empty()) {
                store_.ReplaceTurns(id, transcript.turns);
                note_input = std::move(transcript.turns);
            }
            stage("transcript sealed");
            if (import_progress_) import_progress_(ImportStage::kFinalising, 1.0);
            // Update the voice print only when a doctor was named and, with a note writer, only
            // after the note is accepted
            if (transcript.doctor_cluster >= 0 && learn && learn_anchor_) {
                doctor_voiceprint = diariser_.DoctorVoiceprint(
                    session_audio_, transcript.diarised.slices, transcript.doctor_cluster);
                if (note_writer_ == nullptr) {
                    diariser_.AccrueVoiceprint(doctor_voiceprint);
                    doctor_voiceprint.clear();
                }
            }
            stage("anchor accrued");
        } catch (const std::exception& e) {
            log::Printf("clinicavt-engine: transcription failed: %s\n", e.what());
        } catch (...) {
            log::Printf("clinicavt-engine: transcription failed\n");
        }
    }
    // Clear leftover capture state and cuts so nothing leaks into the next session
    diariser_.DiscardCapture();
    (void)transcriber_.TakeClipCuts();
    session_audio_.clear();
    session_audio_.shrink_to_fit();
    // Honour a cancel that arrived during transcription
    if (outcome == Outcome::kFinalise && import_cancel_) {
        outcome = Outcome::kCancel;
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_finalised_ == id) last_finalised_.clear();
    }
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
    // Delete the resumed-from session; its audio was replayed into this one
    std::string resumed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resumed = std::exchange(resumed_from_, {});
    }
    if (!resumed.empty()) {
        try {
            store_.Delete(resumed);
        } catch (...) {  // NOLINT(bugprone-empty-catch) the session stays, as if kept
        }
    }
    if (outcome == Outcome::kFinalise && note_writer_ != nullptr) {
        stage("note lane started");
        note_lane_.WriteNote(id, std::move(note_input),
                             [this, print = std::move(doctor_voiceprint)] {
                                 if (!print.empty()) diariser_.AccrueVoiceprint(print);
                             });
    }
    return outcome;
}

// Empty when the session has no note or cannot be read
std::string SessionController::StoredNote(const store::SessionId& id) const {
    try {
        return store_.ReadDocument(id, store::DocumentKind::kNote).text;
    } catch (...) {
        return {};
    }
}

}  // namespace clinicavt::session
