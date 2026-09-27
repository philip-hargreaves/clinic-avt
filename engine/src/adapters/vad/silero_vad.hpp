#pragma once

#include <openvino/openvino.hpp>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "ports/streaming_vad.hpp"

namespace clinicavt::audio {

// Silero through the model store, on CPU: ~0.1 ms per hop,
// so it runs inline on the audio pipeline, behind the capture ring
class SileroVad : public IStreamingVad {
   public:
    SileroVad(const models::ModelStore& store, models::OvRuntime& runtime);

    float SpeechProbability(std::span<const float> hop) override;
    void Reset() override;

   private:
    // The model wants 64 samples of left context ahead of each 512-sample hop
    static constexpr std::size_t kContext = 64;

    ov::InferRequest request_;
    ov::Tensor state_{ov::element::f32, {2, 1, 128}};
    std::array<float, kContext> context_{};
};

}  // namespace clinicavt::audio
