#pragma once

#include <functional>
#include <memory>
#include <span>
#include <utility>

#include "adapters/models/deferred_load.hpp"
#include "ports/streaming_vad.hpp"

namespace clinicavt::audio {

// VAD loaded in the background. A failed load throws on the first probability
class DeferredVad : public IStreamingVad {
   public:
    explicit DeferredVad(std::function<std::unique_ptr<IStreamingVad>()> build,
                         metrics::Registry* metrics = nullptr)
        : inner_("vad", std::move(build), metrics) {}

    float SpeechProbability(std::span<const float> hop) override {
        return inner_.Get().SpeechProbability(hop);
    }

    // A newly loaded model is already reset
    void Reset() override {
        if (inner_.Loaded()) {
            inner_.Get().Reset();
        }
    }

    bool Ready() const override {
        return inner_.Settled();
    }

   private:
    models::DeferredLoad<IStreamingVad> inner_;
};

}  // namespace clinicavt::audio
