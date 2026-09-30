#include "core/session/session_controller.hpp"

#include <exception>
#include <memory>
#include <mutex>
#include <utility>

#include "core/audio/resume_source.hpp"
#include "core/common/log.hpp"

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
      note_writer_(note_writer),
      metrics_(metrics),
      settle_timeout_(settle_timeout),
      note_lane_(note_writer, store, events, min_note_words),
      enrolment_(factory_, vad, diariser, events),
      capture_(state_, events, store, transcriber, diariser, note_writer, note_lane_,
               diar_advance_frames,
               [this](const audio::SourceEnd& end) {
                   finaliser_.Finish(Outcome::kAbandon);
                   events_.OnInterrupted(end.reason, end.detail);
               }),
      finaliser_(state_, capture_, note_lane_, events, store, store, transcriber, diariser,
                 note_writer, metrics),
      import_(state_, finaliser_, note_writer, metrics),
      review_(state_, note_lane_, store, store) {
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
        std::lock_guard<std::mutex> lock(state_.mutex);
        if (!resumed_audio.empty()) {
            if (replay.has_value()) {
                replay->start_frame += resumed_audio.size();
            }
            capture_.SetSource(std::make_unique<audio::ResumeSource>(std::move(resumed_audio),
                                                                     factory_(replay, mic.id)));
        } else {
            capture_.SetSource(factory_(replay, mic.id));
        }
    }
    capture_.Launch();
    if (metrics_ != nullptr) {
        metrics_->BeginSession(replay.has_value(), replay.has_value() ? replay->speed : 0.0);
    }

    std::unique_lock<std::mutex> lock(state_.mutex);
    state_.cv.wait_for(lock, settle_timeout_, [this] { return state_.got_audio || state_.ended; });
    if (state_.got_audio) {
        return true;
    }
    lock.unlock();
    Cancel();  // no audio arrived, so erase the session

    std::lock_guard<std::mutex> relock(state_.mutex);
    if (state_.end.reason == audio::SourceEndReason::kStopped) {
        state_.end = {audio::SourceEndReason::kFailed, "no audio arrived before the deadline"};
    }
    return false;
}

store::SessionId SessionController::Import(ImportRead read, const std::string& started_at,
                                           bool retain, ImportReport report) {
    if (Importing()) {
        return {};
    }
    import_.Join();
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
        std::lock_guard<std::mutex> lock(state_.mutex);
        id = state_.session_id;
        state_.importing = true;
        capture_.SetSource(nullptr);  // stop the last recording's source feeding the import
    }
    import_.Launch(std::move(read), std::move(report));
    return id;
}

bool SessionController::Importing() const {
    std::lock_guard<std::mutex> lock(state_.mutex);
    return state_.importing;
}

bool SessionController::Claim() {
    std::lock_guard<std::mutex> lock(state_.mutex);
    if (state_.running || enrolment_.Running()) {
        return false;
    }
    state_.running = true;
    state_.reviewing = false;  // a new session ends any review
    state_.got_audio = false;
    state_.ended = false;
    state_.stop_requested = false;
    state_.diar_ticks = 0;
    state_.lost_frames = 0;
    state_.end = {};
    capture_.Reset();
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
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.session_id = id;
        state_.resumed_from = resumed_from;
        state_.session_audio.clear();
        return resumed_audio;
    } catch (const std::exception& e) {
        log::Printf("clinicavt-engine: session start failed: %s\n", e.what());
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.running = false;
        state_.end = {audio::SourceEndReason::kFailed,
                      std::string("session setup failed: ") + e.what()};
        return std::nullopt;
    }
}

void SessionController::Stop() {
    import_.Join();
    // Re-warm the note model during finalise; a long session may have evicted it
    if (note_writer_ != nullptr && Running()) {
        note_writer_->Prepare();
    }
    capture_.End();
    finaliser_.Finish(Outcome::kFinalise);
}

void SessionController::Cancel() {
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        if (state_.importing) {
            import_.RequestCancel();
            return;
        }
    }
    capture_.End();
    finaliser_.Finish(Outcome::kCancel);
}

bool SessionController::Running() const {
    return state_.Running();
}

bool SessionController::Busy() const {
    return Running() || note_lane_.Busy();
}

bool SessionController::StartEnrolment(double seconds, const MicSelection& mic,
                                       double min_speech_s) {
    std::lock_guard<std::mutex> lock(state_.mutex);
    if (state_.running) return false;
    return enrolment_.Start(seconds, mic, min_speech_s);
}

void SessionController::CancelEnrolment() {
    enrolment_.Cancel();
}

void SessionController::FinishEnrolment() {
    enrolment_.Finish();
}

audio::SourceEnd SessionController::LastEnd() const {
    std::lock_guard<std::mutex> lock(state_.mutex);
    return state_.end;
}

store::SessionId SessionController::LastFinalised() const {
    return state_.LastFinalised();
}

bool SessionController::Open(const store::SessionId& id) {
    return review_.Open(id);
}

void SessionController::Close() {
    review_.Close();
}

store::SessionId SessionController::CurrentSession() const {
    return state_.Current();
}

void SessionController::SetNoteOptions(note::NoteOptions options) {
    note_lane_.SetOptions(options);
}

bool SessionController::HasNoteWriter() const {
    return note_lane_.Available();
}

bool SessionController::RegenerateNote(note::NoteOptions options) {
    return review_.RegenerateNote(options);
}

bool SessionController::WriteSummary(store::SessionId id) {
    return review_.WriteSummary(std::move(id));
}

bool SessionController::RegeneratePatient() {
    return review_.RegeneratePatient();
}

}  // namespace clinicavt::session
