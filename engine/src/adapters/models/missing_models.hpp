#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "ports/diariser.hpp"
#include "ports/streaming_vad.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::models {

// Placeholders for roles that are not installed when the engine runs without --scripted.
// Consultations are refused before reaching them, so any call is a bug and throws
class MissingTranscriber : public asr::ITranscriber {
   public:
    std::vector<asr::Turn> DecodeClipChunks(std::span<const float>, std::uint64_t) override {
        throw std::logic_error("the speech recognition model is not installed");
    }
};

class MissingVad : public audio::IStreamingVad {
   public:
    float SpeechProbability(std::span<const float>) override {
        throw std::logic_error("the speech detection model is not installed");
    }

    void Reset() override {}
};

class MissingDiariser : public diar::IDiariser {
   public:
    diar::DiariseResult Diarise(std::span<const float>) override {
        throw std::logic_error("the speaker models are not installed");
    }

    std::vector<double> AnchorSimilarities(std::span<const float>,
                                           const std::vector<diar::LabelledSlice>&, int) override {
        throw std::logic_error("the speaker models are not installed");
    }
};

}  // namespace clinicavt::models
