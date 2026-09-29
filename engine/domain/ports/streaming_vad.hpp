#pragma once

#include <cstddef>
#include <span>

namespace clinicavt::audio {

// 32 ms at 16 kHz, Silero's native hop
inline constexpr std::size_t kVadHopFrames = 512;

// Streaming VAD that returns one probability per hop and keeps recurrent state across hops in a
// session
class IStreamingVad {
   public:
    virtual ~IStreamingVad() = default;

    // hop must be exactly kVadHopFrames
    virtual float SpeechProbability(std::span<const float> hop) = 0;

    virtual void Reset() = 0;

    // False while loading. Callers buffer audio meanwhile and do not block
    virtual bool Ready() const {
        return true;
    }
};

}  // namespace clinicavt::audio
