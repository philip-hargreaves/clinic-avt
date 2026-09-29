#include "core/session/enrolment.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>

#include "core/audio/voice_enrolment.hpp"

namespace clinicavt::session {

Enrolment::Enrolment(const SourceFactory& factory, audio::IStreamingVad& vad,
                     diar::IDiariser& diariser, ISessionEvents& events)
    : factory_(factory), vad_(vad), diariser_(diariser), events_(events) {}

Enrolment::~Enrolment() {
    Cancel();
    Join();
}

bool Enrolment::Start(double seconds, const MicSelection& mic, double min_speech_s) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return false;
        running_ = true;
        cancel_ = false;
        finish_ = false;
    }
    Join();
    thread_ = std::thread([this, seconds, mic, min_speech_s] { Run(seconds, mic, min_speech_s); });
    return true;
}

void Enrolment::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    cancel_ = true;
    if (source_) source_->RequestStop();
}

void Enrolment::Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    finish_ = true;
    if (source_) source_->RequestStop();
}

bool Enrolment::Running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void Enrolment::Join() {
    if (thread_.joinable()) thread_.join();
}

void Enrolment::Run(double seconds, const MicSelection& mic, double min_speech_s) {
    std::string why;
    double speech_s = 0.0;
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            source_ = factory_(std::nullopt, mic.id);
        }
        const auto frames = static_cast<std::uint64_t>(seconds * audio::kSampleRate);
        audio::EnrolmentSink sink(
            vad_, frames,
            [this] {
                std::lock_guard<std::mutex> lock(mutex_);
                if (source_) source_->RequestStop();
            },
            [this](const audio::EnrolProgress& progress) { events_.OnEnrolProgress(progress); });
        try {
            source_->Run(sink);
        } catch (const std::exception& e) {
            sink.OnEnd({audio::SourceEndReason::kFailed, e.what()});
        }
        auto capture = sink.Take();
        speech_s = static_cast<double>(capture.speech.size()) / audio::kSampleRate;
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled = cancel_ && !finish_;
        }
        why = audio::EnrolRejection(capture, cancelled, min_speech_s);
        if (why.empty()) {
            const auto voiceprint = diariser_.EmbedVoice(capture.speech);
            if (voiceprint.empty()) {
                why = "could not build a voiceprint from the recording";
            } else {
                const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                diariser_.ReplaceAnchor(voiceprint, static_cast<std::uint64_t>(now));
            }
        }
    } catch (const std::exception& e) {
        why = std::string("microphone unavailable: ") + e.what();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source_.reset();
        running_ = false;
    }
    events_.OnEnrolDone(why.empty(), why, speech_s);
}

}  // namespace clinicavt::session
