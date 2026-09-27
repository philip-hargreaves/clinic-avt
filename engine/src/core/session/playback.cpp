#include "core/session/playback.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/common/iso8601.hpp"
#include "ports/audio_source.hpp"

namespace clinicavt::session {
namespace {

// A hook failing never ends the playback
void Call(const std::function<void(const store::SessionId&)>& hook, const store::SessionId& id,
          const char* what) {
    if (!hook) return;
    try {
        hook(id);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt-engine: playback %s hook failed: %s\n", what, e.what());
    }
}

}  // namespace

Playback::Playback(ISessionEvents& events, store::ISessionStore& store, Hooks hooks,
                   PlaybackPacing pacing)
    : events_(events), store_(store), hooks_(std::move(hooks)), pacing_(pacing) {}

Playback::~Playback() {
    Cancel();
}

bool Playback::Start(const store::SessionId& source) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ != Phase::kIdle) return false;
    }
    Join();
    Copy copy;
    try {
        copy = MakeCopy(source);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt-engine: playback refused: %s\n", e.what());
        return false;
    }
    std::fprintf(stderr, "clinicavt-engine: playback of %s as demo %s\n", source.c_str(),
                 copy.id.c_str());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = Phase::kListening;
        stop_ = cancel_ = paused_ = false;
        current_ = copy.id;
    }
    thread_ = std::thread([this, copy = std::move(copy)] { Run(copy); });
    return true;
}

void Playback::Stop() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (phase_ == Phase::kIdle) return;
    stop_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return phase_ == Phase::kWriting || phase_ == Phase::kIdle; });
}

void Playback::Cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancel_ = true;
        cv_.notify_all();
    }
    Join();
}

void Playback::SetPaused(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = paused;
    cv_.notify_all();
}

bool Playback::Listening() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return phase_ == Phase::kListening;
}

bool Playback::Active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return phase_ != Phase::kIdle;
}

store::SessionId Playback::Current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

// A finalised, demo-flagged twin of the source, dated as if newly recorded
Playback::Copy Playback::MakeCopy(const store::SessionId& source) {
    using store::DocumentKind;
    const auto turns = store_.ReadTurns(source);
    const auto note = store_.ReadDocument(source, DocumentKind::kNote);
    if (turns.empty() || note.text.empty()) {
        throw std::runtime_error("no finished transcript and note to play back");
    }
    Copy copy;
    for (const auto& turn : turns) {
        copy.audio_seconds =
            std::max(copy.audio_seconds,
                     static_cast<double>(turn.first_frame + turn.frame_count) / audio::kSampleRate);
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    store::SessionSeed seed;
    seed.started_at =
        Iso8601(now - std::chrono::seconds(static_cast<long long>(copy.audio_seconds)));
    seed.ended_at = Iso8601(now);
    seed.turns = turns;
    copy.id = store_.Seed(seed);
    copy.note = note.text;
    store_.SaveDocument(copy.id, DocumentKind::kNote, note);
    if (const auto label = store_.ReadDocument(source, DocumentKind::kLabel); !label.text.empty()) {
        store_.SaveDocument(copy.id, DocumentKind::kLabel, label);
    }
    if (const auto patient = store_.ReadDocument(source, DocumentKind::kPatient);
        !patient.text.empty()) {
        copy.patient = patient.text;
        store_.SaveDocument(copy.id, DocumentKind::kPatient, patient);
    }
    return copy;
}

void Playback::Run(const Copy& copy) {
    Listen(copy.audio_seconds);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || cancel_; });
        if (cancel_) {
            lock.unlock();
            Erase(copy.id);
            Finish();
            return;
        }
        phase_ = Phase::kFinalising;
    }
    Finalise(copy.id);
    Write(copy);
    Finish();
}

// Level readings carry the racing clock on a speech-shaped envelope, since a
// meter pinned at one height reads as broken
void Playback::Listen(double audio_seconds) {
    const int ticks = std::max<int>(1, static_cast<int>(pacing_.listen / pacing_.tick));
    for (int i = 1; i <= ticks; ++i) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !paused_ || stop_ || cancel_; });
            if (stop_ || cancel_) return;
        }
        const auto beat = static_cast<double>(i);
        const float level =
            static_cast<float>(0.25 + 0.55 * std::fabs(std::sin(beat * 0.6)) *
                                          (0.6 + 0.4 * std::fabs(std::sin(beat * 0.13))));
        events_.OnPlaybackLevel({level, false}, audio_seconds * i / ticks);
        std::this_thread::sleep_for(pacing_.tick);
    }
}

void Playback::Finalise(const store::SessionId& id) {
    events_.OnProgress("transcript");
    std::this_thread::sleep_for(pacing_.transcript);
    // No per-turn re-decode stage: the shell labels it as the transcript again
    events_.OnProgress("speakers");
    std::this_thread::sleep_for(pacing_.speakers);
    Call(hooks_.finalised, id, "review");
    std::lock_guard<std::mutex> lock(mutex_);
    phase_ = Phase::kWriting;
    cv_.notify_all();
}

void Playback::Write(const Copy& copy) {
    std::this_thread::sleep_for(pacing_.first_token);
    Stream(copy.note, [this](const std::string& text) { events_.OnNotePartial(text); });
    if (cancel_) return;
    events_.OnNoteReady(copy.note);
    std::this_thread::sleep_for(pacing_.guidance);
    Call(hooks_.guidance, copy.id, "guidance");
    std::this_thread::sleep_for(pacing_.first_token);
    Stream(copy.patient, [this](const std::string& text) { events_.OnPatientPartial(text); });
    if (cancel_) return;
    events_.OnPatientReady(copy.patient);
}

// Growing prefixes on word boundaries at the partial cadence
void Playback::Stream(const std::string& text,
                      const std::function<void(const std::string&)>& emit) {
    std::vector<std::size_t> ends;
    for (std::size_t i = 1; i < text.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(text[i])) != 0 &&
            std::isspace(static_cast<unsigned char>(text[i - 1])) == 0) {
            ends.push_back(i);
        }
    }
    if (text.empty()) return;
    ends.push_back(text.size());
    const auto over = std::chrono::milliseconds(static_cast<long long>(
        1000.0 * static_cast<double>(ends.size()) / pacing_.words_per_second));
    const auto steps = std::max<std::size_t>(1, static_cast<std::size_t>(over / pacing_.tick));
    const auto per = std::max<std::size_t>(1, (ends.size() + steps - 1) / steps);
    const auto pause = over / static_cast<int>((ends.size() + per - 1) / per);
    for (std::size_t i = per - 1; i < ends.size(); i += per) {
        if (cancel_) return;
        emit(text.substr(0, ends[i]));
        std::this_thread::sleep_for(pause);
    }
    if (ends.size() % per != 0) emit(text);
}

void Playback::Erase(const store::SessionId& id) {
    try {
        store_.Delete(id);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt-engine: playback copy not erased: %s\n", e.what());
    }
}

void Playback::Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    phase_ = Phase::kIdle;
    cv_.notify_all();
}

void Playback::Join() {
    if (thread_.joinable()) thread_.join();
}

}  // namespace clinicavt::session
