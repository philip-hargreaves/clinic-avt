#pragma once

#include "ports/streaming_vad.hpp"

namespace clinicavt::audio {

// Fallback when no VAD model is staged. Everything is speech, so the endpointer only makes
// capped cuts
class PassthroughVad : public IStreamingVad {
   public:
    float SpeechProbability(std::span<const float>) override {
        return 1.0f;
    }

    void Reset() override {}
};

}  // namespace clinicavt::audio
