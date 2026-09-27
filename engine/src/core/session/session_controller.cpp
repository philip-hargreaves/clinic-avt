#include "core/session/session_controller.hpp"

#include <cstdio>
#include <exception>
#include <utility>

#include "core/audio/buffered_sink.hpp"
#include "core/audio/resume_source.hpp"
#include "core/audio/voice_enrolment.hpp"
#include "core/diarisation/tidy_transcript.hpp"
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_ || enrolment_.Running()) {
            return false;
        }
        running_ = true;
        reviewing_ = false;  // Record wins over a review
        got_audio_ = false;
        ended_ = false;
        stop_requested_ = false;
        diar_stop_ = false;
        lost_frames_ = 0;
        end_ = {};
        meter_ = audio::LevelMeter{};
    }
    std::vector<float> resumed_audio;
    try {
        if (!resume_from.empty()) {
            resumed_audio = store_.ReadAudio(resume_from);
            std::fprintf(stderr, "clinicavt-engine: resuming %s with %.1f s of stored audio\n",
                         resume_from.c_str(),
                         static_cast<double>(resumed_audio.size()) / audio::kSampleRate);
        }
        store_.EraseUnretained();  // the previous consultation is left
        store::SessionMeta meta{audio::kSampleRate, "", ""};
        if (!replay.has_value()) {
            // What was actually opened, so a default fallback is on record
            meta.device_id = mic.id;
            meta.device_name = mic.name;
        }
        meta.retain = retain;
        const store::SessionId id = store_.Begin(meta);
        std::lock_guard<std::mutex> lock(mutex_);
        session_id_ = id;
        resumed_from_ = resume_from;
        note_prepared_ = false;
        session_audio_.clear();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt-engine: session start failed: %s\n", e.what());
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        end_ = {audio::SourceEndReason::kFailed, std::string("session setup failed: ") + e.what()};
        return false;
    }
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
    Cancel();  // a session that never produced audio leaves no trace

    std::lock_guard<std::mutex> relock(mutex_);
    if (end_.reason == audio::SourceEndReason::kStopped) {
        end_ = {audio::SourceEndReason::kFailed, "no audio arrived before the deadline"};
    }
    return false;
}

void SessionController::Stop() {
    // Re-warm in parallel with finalise, since a long session may have evicted
    if (note_writer_ != nullptr && Running()) {
        note_writer_->Prepare();
    }
    EndCapture();
    FinishSession(Outcome::kFinalise);
}

void SessionController::Cancel() {
    EndCapture();
    FinishSession(Outcome::kCancel);
}

void SessionController::SetPaused(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source_ && running_) source_->SetPaused(paused);
}

void SessionController::SetMonitor(bool monitor) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source_ && running_) source_->SetMonitor(monitor);
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
            // Only a fresh capture too short for a note goes. A reviewed session is a kept record
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
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }
    if (!Running()) {
        try {
            store_.EraseUnretained();
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }
}

store::SessionId SessionController::CurrentSession() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_id_;
}

void SessionController::SetNoteOptions(note::NoteOptions options) {
    note_lane_.SetOptions(std::move(options));
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
    note_lane_.SetOptions(std::move(options));
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
    // Only an interruption decides its own outcome. A stopped source leaves
    // keep-or-discard to Stop or Cancel
    if (interrupted) {
        controller.FinishSession(Outcome::kAbandon);
        controller.events_.OnInterrupted(end.reason, end.detail);
    }
}

// The source's thread only fills the ring and the pipeline runs behind it. An
// escape from a thread function is std::terminate, so nothing escapes
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

// Diarisation's causal work, spread over the recording. The heavy Advance
// runs outside the lock, off the pipeline thread
void SessionController::DiarLoop() {
    // Accelerated replay delivers audio faster than real time. A wall floor
    // keeps the tick rate sane at any speed
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
        // Deferred until whisper is decoding so the GPU never compiles two
        // models at once, still minutes ahead of any real stop
        if (!note_prepared_ && note_writer_ != nullptr) {
            note_prepared_ = true;
            note_writer_->Prepare();
        }
        try {
            const auto decode = [this](std::span<const float> clip,
                                       std::uint64_t first) -> std::vector<asr::Turn> {
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    // A stop must not wait behind a speculation pass
                    if (diar_stop_) return {};
                }
                return transcriber_.DecodeClipChunks(clip, first);
            };
            diariser_.Advance(audio, decode);
            // This tick's chunk edges re-slice the audio. The pieces decode in
            // the same tick, so a stop never waits for them
            const auto cuts = transcriber_.TakeClipCuts();
            if (!cuts.empty()) {
                diariser_.AddCutPoints(cuts);
                diariser_.Advance(audio, decode);
            }
            // The note host extends its KV over the settled opening between
            // whisper decodes. The finalise tidies its turns, so the prefix
            // must read the same
            if (note_writer_ != nullptr) {
                auto guess = diar::TidyTranscript(diariser_.SpeculativeTranscript());
                if (!guess.empty()) note_writer_->Prefill(guess, note_lane_.Options());
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "clinicavt-engine: capture tick failed: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "clinicavt-engine: capture tick failed\n");
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

// Stop, cancel and abandon all end here. The store outcome always holds even
// if the bookkeeping around it fails
void SessionController::FinishSession(Outcome outcome) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_id_.empty()) {
            return;
        }
    }
    // No capture work may run once finalise starts. Stage timings let a slow
    // finalise name its stage
    const auto finalise_start = std::chrono::steady_clock::now();
    const auto stage = [this, &finalise_start](const char* name) {
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - finalise_start)
                .count();
        std::fprintf(stderr, "clinicavt-engine: finalise %s at %.1f s\n", name, seconds);
        if (metrics_ != nullptr) {
            metrics_->RecordStage(name, seconds);
        }
    };
    JoinDiarThread();
    stage("capture joined");
    std::fprintf(stderr, "clinicavt-engine: session audio %.1f s, %d capture ticks\n",
                 static_cast<double>(session_audio_.size()) / audio::kSampleRate, diar_ticks_);
    if (metrics_ != nullptr) {
        metrics_->RecordSession(static_cast<double>(session_audio_.size()) / audio::kSampleRate,
                                lost_frames_, diar_ticks_);
    }
    // Capture decodes a few spans per tick and can lag. The rest decodes now,
    // so the cuts reach the diariser at every replay speed
    if (outcome == Outcome::kFinalise) {
        try {
            diariser_.Settle(session_audio_,
                             [this](std::span<const float> clip, std::uint64_t first) {
                                 return transcriber_.DecodeClipChunks(clip, first);
                             });
            const auto cuts = transcriber_.TakeClipCuts();
            if (!cuts.empty()) diariser_.AddCutPoints(cuts);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "clinicavt-engine: capture settle failed: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "clinicavt-engine: capture settle failed\n");
        }
        stage("capture settled");
        events_.OnProgress("transcript");
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
        return;
    }
    // The note lane's input is the attributed transcript. A diarisation
    // failure leaves it empty, so the note is refused as too thin and the
    // session is never lost
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
            // The print learns only from named sessions, and only once the
            // note lane agrees this was a consultation
            if (transcript.doctor_cluster >= 0 && learn_anchor_) {
                doctor_voiceprint = diariser_.DoctorVoiceprint(
                    session_audio_, transcript.diarised.slices, transcript.doctor_cluster);
                if (note_writer_ == nullptr) {
                    diariser_.AccrueVoiceprint(doctor_voiceprint);
                    doctor_voiceprint.clear();
                }
            }
            stage("anchor accrued");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "clinicavt-engine: transcription failed: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "clinicavt-engine: transcription failed\n");
        }
    }
    // Capture state a finalise did not consume must not leak into the next
    // session (cancel, abandon, a diarisation failure)
    diariser_.DiscardCapture();
    session_audio_.clear();
    session_audio_.shrink_to_fit();
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
    // The resumed-from session is superseded: everything it held flowed into
    // this one before any outcome could be reached
    std::string resumed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resumed = std::exchange(resumed_from_, {});
    }
    if (!resumed.empty()) {
        try {
            store_.Delete(resumed);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }
    if (outcome == Outcome::kFinalise && note_writer_ != nullptr) {
        stage("note lane started");
        note_lane_.WriteNote(id, std::move(note_input),
                             [this, print = std::move(doctor_voiceprint)] {
                                 if (!print.empty()) diariser_.AccrueVoiceprint(print);
                             });
    }
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
