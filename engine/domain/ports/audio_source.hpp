#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace clinicavt::audio {

// The pipeline's one format: 16 kHz mono float32
inline constexpr int kSampleRate = 16000;

enum class SourceEndReason {
    kCompleted,   // The source ran out of audio
    kStopped,     // RequestStop was honoured
    kDeviceLost,  // The device vanished mid-capture
    kFailed,      // The source broke, and detail says how
};

struct SourceEnd {
    SourceEndReason reason;
    std::string detail;
};

struct LevelReading {
    float level = 0.0F;  // 0..1 over a dBFS scale from the floor up
    bool clipped = false;
};

struct EnrolProgress {
    double elapsed_s = 0.0;
    double speech_s = 0.0;
    LevelReading level;
};

// Called on the source thread
class IAudioSink {
   public:
    virtual ~IAudioSink() = default;

    // lost_frames counts audio before this packet that was never delivered. It is zero for a
    // complete recording
    virtual void OnAudio(std::span<const float> frames, std::uint64_t lost_frames) = 0;

    // Always the last call, whatever the reason
    virtual void OnEnd(const SourceEnd& end) = 0;
};

// One capture stream. Run blocks until it ends. RequestStop is thread-safe and
// ends it as kStopped
class IAudioSource {
   public:
    virtual ~IAudioSource() = default;

    virtual void Run(IAudioSink& sink) = 0;
    virtual void RequestStop() = 0;
};

}  // namespace clinicavt::audio
