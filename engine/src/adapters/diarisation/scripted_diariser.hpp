#pragma once

#include <span>
#include <vector>

#include "ports/diariser.hpp"

namespace clinicavt::diar {

// CI stand-in when no speaker models are staged. The whole recording is one speaker
class ScriptedDiariser : public IDiariser {
   public:
    DiariseResult Diarise(std::span<const float> audio) override {
        DiariseResult result;
        result.cluster_count = 1;
        result.slices = {{0, audio.size(), 0}};
        return result;
    }

    std::vector<double> AnchorSimilarities(std::span<const float>,
                                           const std::vector<LabelledSlice>&, int) override {
        return {};
    }

    ICaptureDiarisation* Capture() override {
        return nullptr;
    }

    IVoiceprints* Voiceprints() override {
        return nullptr;
    }
};

}  // namespace clinicavt::diar
