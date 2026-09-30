#pragma once

#include <openvino/openvino.hpp>
#include <span>
#include <vector>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"

namespace clinicavt::diar {

inline constexpr std::size_t kEmbeddingDims = 192;

// ERes2NetV2 on CPU, where INT8 is lossless and 1.9x faster. The GPU gives no gain
class SpeakerEmbedder {
   public:
    SpeakerEmbedder(const models::ModelStore& store, models::OvRuntime& runtime);

    // Unit-norm voiceprint of a speech slice. Input is float32 [-1, 1] mono 16 kHz
    std::vector<float> Embed(std::span<const float> audio);

   private:
    ov::InferRequest request_;
};

}  // namespace clinicavt::diar
