#include "adapters/note/text_pipeline.hpp"

#include <openvino/genai/llm_pipeline.hpp>
#include <openvino/genai/visual_language/pipeline.hpp>
#include <utility>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"

namespace clinicavt::note {

namespace {

using models::CompileProperties;

ov::genai::StreamerVariant Wrap(const TextPipeline::Streamer& streamer) {
    if (!streamer) return std::monostate{};
    return std::function<ov::genai::StreamingStatus(std::string)>(streamer);
}

class LlmTextPipeline : public TextPipeline {
   public:
    LlmTextPipeline(const models::ModelInfo& info, const std::string& device)
        : pipeline_(info.dir, device, CompileProperties(info)) {}

    Result Generate(const std::string& prompt, const ov::genai::GenerationConfig& config,
                    const Streamer& streamer) override {
        ov::genai::DecodedResults result = pipeline_.generate(prompt, config, Wrap(streamer));
        return {result.perf_metrics.get_num_input_tokens()};
    }

   private:
    ov::genai::LLMPipeline pipeline_;
};

// The multimodal export used text-only. The vision towers still load
class VlmTextPipeline : public TextPipeline {
   public:
    VlmTextPipeline(const models::ModelInfo& info, const std::string& device)
        : pipeline_(info.dir, device, CompileProperties(info)) {}

    Result Generate(const std::string& prompt, const ov::genai::GenerationConfig& config,
                    const Streamer& streamer) override {
        ov::genai::VLMDecodedResults result =
            pipeline_.generate(prompt, std::vector<ov::Tensor>{}, config, Wrap(streamer));
        return {result.perf_metrics.get_num_input_tokens()};
    }

   private:
    ov::genai::VLMPipeline pipeline_;
};

}  // namespace

std::unique_ptr<TextPipeline> MakeTextPipeline(const models::ModelInfo& info,
                                               const std::string& device) {
    if (info.pipeline == "vlm") {
        return std::make_unique<VlmTextPipeline>(info, device);
    }
    return std::make_unique<LlmTextPipeline>(info, device);
}

}  // namespace clinicavt::note
