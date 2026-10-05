#include "core/session/capture_lane.hpp"

#include <chrono>
#include <exception>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/audio/buffered_sink.hpp"
#include "core/common/log.hpp"
#include "core/diarisation/tidy_transcript.hpp"

namespace clinicavt::session {

CaptureLane::CaptureLane(SessionState& state, ICaptureEvents& events, store::IRecordingStore& store,
                         asr::ITranscriber& transcriber, diar::IDiariser& diariser,
                         note::INoteWriter* note_writer, const NoteLane& note_lane,
                         std::uint64_t diar_advance_frames, Interrupted interrupted)
    : state_(state),
      events_(events),
      store_(store),
      transcriber_(transcriber),
      diariser_(diariser),
      note_writer_(note_writer),
      note_lane_(note_lane),
      diar_advance_frames_(diar_advance_frames),
      interrupted_(std::move(interrupted)),
      diarisation_(state.mutex, state.cv) {}

void CaptureLane::Reset() {
    diarisation_.ClearStop();
    meter_ = audio::LevelMeter{};
    note_prepared_ = false;
}

void CaptureLane::SetSource(std::unique_ptr<audio::IAudioSource> source) {
    source_ = std::move(source);
}

// Diarisation starts first because an interrupted capture stops it from the capture thread
void CaptureLane::Launch() {
    diarisation_.Start([this] { DiarLoop(); });
    worker_ = std::thread([this] { GuardedRun(); });
}

void CaptureLane::End() {
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.stop_requested = true;
    }
    if (source_) {
        source_->RequestStop();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(state_.mutex);
    state_.running = false;
}

void CaptureLane::JoinDiarisation() {
    diarisation_.Stop();
}

void CaptureLane::PipelineSink::OnAudio(std::span<const float> frames, std::uint64_t lost_frames) {
    SessionState& state = lane.state_;
    store::SessionId id;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.lost_frames += lost_frames;
        state.got_audio = true;
        id = state.session_id;
        // Under the lock because the diarisation thread copies it
        if (!id.empty()) {
            state.session_audio.insert(state.session_audio.end(), frames.begin(), frames.end());
        }
    }
    state.cv.notify_all();
    if (!id.empty()) {
        lane.store_.Append(id, frames, lost_frames);
    }
    for (const auto& reading : lane.meter_.Push(frames)) {
        lane.events_.OnLevel(reading);
    }
}

void CaptureLane::PipelineSink::OnEnd(const audio::SourceEnd& end) {
    SessionState& state = lane.state_;
    bool interrupted = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.end = end;
        state.ended = true;
        interrupted = state.got_audio && !state.stop_requested &&
                      (end.reason == audio::SourceEndReason::kDeviceLost ||
                       end.reason == audio::SourceEndReason::kFailed);
    }
    state.cv.notify_all();
    // Only an interruption finishes the session here. Stop and Cancel finish the others
    if (interrupted) {
        lane.interrupted_(end);
    }
}

// Catches everything, since an exception leaving a thread function calls std::terminate
void CaptureLane::GuardedRun() {
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
void CaptureLane::DiarLoop() {
    // Minimum wall-clock gap between ticks, for faster-than-real-time replay
    constexpr auto kMinTickGap = std::chrono::seconds(1);
    std::vector<float> audio;
    std::unique_lock<std::mutex> lock(state_.mutex);
    for (;;) {
        state_.cv.wait(lock, [this, &audio] {
            return diarisation_.Stopping() ||
                   state_.session_audio.size() >= audio.size() + diar_advance_frames_;
        });
        if (diarisation_.Stopping()) {
            return;
        }
        audio = state_.session_audio;
        ++state_.diar_ticks;
        lock.unlock();
        // Deferred until whisper is decoding, so the GPU never compiles two models at once
        if (!note_prepared_ && note_writer_ != nullptr) {
            note_prepared_ = true;
            note_writer_->Prepare();
        }
        try {
            // Speculative decodes are skipped once stopping, and one already waiting for the GPU
            // is dropped, so Stop and Cancel never wait for another model's load
            const auto stopping = [this] {
                std::lock_guard<std::mutex> guard(state_.mutex);
                return diarisation_.Stopping();
            };
            const auto decode = [this, &stopping](std::span<const float> clip,
                                                  std::uint64_t first) -> std::vector<asr::Turn> {
                if (stopping()) return {};
                return transcriber_.DecodeClipChunks(clip, first, stopping);
            };
            auto* const capture = diariser_.Capture();
            if (capture != nullptr) capture->Advance(audio, decode);
            // Apply this tick's chunk-edge cuts and decode the pieces now, to leave less for Stop
            const auto cuts = transcriber_.TakeClipCuts();
            if (!cuts.empty() && capture != nullptr) {
                capture->AddCutPoints(cuts);
                capture->Advance(audio, decode);
            }
            // Prefill the note model's KV cache with the settled transcript, tidied as finalise
            // does so the prefix matches
            if (note_writer_ != nullptr && capture != nullptr) {
                auto guess = diar::TidyTranscript(capture->SpeculativeTranscript());
                if (!guess.empty()) note_writer_->Prefill(guess, note_lane_.Options());
            }
        } catch (const std::exception& e) {
            log::Printf("clinicavt-engine: capture tick failed: %s\n", e.what());
        } catch (...) {
            log::Printf("clinicavt-engine: capture tick failed\n");
        }
        lock.lock();
        state_.cv.wait_for(lock, kMinTickGap, [this] { return diarisation_.Stopping(); });
        if (diarisation_.Stopping()) {
            return;
        }
    }
}

}  // namespace clinicavt::session
