#pragma once

#include <functional>
#include <memory>
#include <openvino/genai/generation_config.hpp>
#include <openvino/genai/streamer_base.hpp>
#include <string>

namespace clinicavt::models {
struct ModelInfo;
}  // namespace clinicavt::models

namespace clinicavt::note {

// One resident model behind the GenAI pipeline its manifest names. It keeps one history, so a
// prompt extending the last costs only the delta and a diverging one re-prefills
class TextPipeline {
   public:
    using Streamer = std::function<ov::genai::StreamingStatus(std::string)>;

    struct Result {
        std::size_t input_tokens = 0;
    };

    virtual ~TextPipeline() = default;

    // Streams pieces to the streamer when given. The streamer's STOP ends the generation
    virtual Result Generate(const std::string& prompt, const ov::genai::GenerationConfig& config,
                            const Streamer& streamer) = 0;
};

// Builds the pipeline the manifest names on the resolved device, with the compile cache next to
// the model and the manifest properties unchanged
std::unique_ptr<TextPipeline> MakeTextPipeline(const models::ModelInfo& info,
                                               const std::string& device);

}  // namespace clinicavt::note
