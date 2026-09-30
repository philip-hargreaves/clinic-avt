#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <thread>

#include "core/audio/level_meter.hpp"
#include "core/common/worker_thread.hpp"
#include "core/session/note_lane.hpp"
#include "core/session/session_state.hpp"
#include "ports/audio_source.hpp"
#include "ports/diariser.hpp"
#include "ports/note_writer.hpp"
#include "ports/session_events.hpp"
#include "ports/session_store.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::session {

// Runs a recording session's capture thread and its diarisation thread
class CaptureLane {
   public:
    // Capture ring size. Covers a first-launch model compile, which stalls the pipeline for a few
    // seconds
    static constexpr std::size_t kCaptureBufferFrames = std::size_t{30} * audio::kSampleRate;

    // Runs on the capture thread when the source fails or loses its device after audio arrived
    using Interrupted = std::function<void(const audio::SourceEnd& end)>;

    CaptureLane(SessionState& state, ICaptureEvents& events, store::IRecordingStore& store,
                asr::ITranscriber& transcriber, diar::IDiariser& diariser,
                note::INoteWriter* note_writer, const NoteLane& note_lane,
                std::uint64_t diar_advance_frames, Interrupted interrupted);
    CaptureLane(const CaptureLane&) = delete;
    CaptureLane& operator=(const CaptureLane&) = delete;

    // Caller holds state.mutex
    void Reset();
    void SetSource(std::unique_ptr<audio::IAudioSource> source);

    void Launch();
    // Stops the source and joins the capture thread. The diarisation thread keeps running until
    // JoinDiarisation
    void End();
    void JoinDiarisation();

   private:
    // Runs on the pipeline thread and feeds the store, the diariser's audio buffer and the level
    // meter
    struct PipelineSink : audio::IAudioSink {
        CaptureLane& lane;

        explicit PipelineSink(CaptureLane& owner) : lane(owner) {}

        void OnAudio(std::span<const float> frames, std::uint64_t lost_frames) override;
        void OnEnd(const audio::SourceEnd& end) override;
    };

    void GuardedRun();
    void DiarLoop();

    SessionState& state_;
    ICaptureEvents& events_;
    store::IRecordingStore& store_;
    asr::ITranscriber& transcriber_;
    diar::IDiariser& diariser_;
    note::INoteWriter* note_writer_;
    const NoteLane& note_lane_;
    std::uint64_t diar_advance_frames_;
    Interrupted interrupted_;
    std::unique_ptr<audio::IAudioSource> source_;
    audio::LevelMeter meter_;
    bool note_prepared_ = false;  // diar thread only
    std::thread worker_;
    WorkerThread diarisation_;
};

}  // namespace clinicavt::session
