#include "adapters/guidance/embedder.hpp"

#include <cmath>
#include <openvino/genai/rag/text_embedding_pipeline.hpp>
#include <openvino/genai/tokenizer.hpp>
#include <stdexcept>
#include <utility>
#include <variant>

#include "adapters/models/model_store.hpp"

namespace clinicavt::guidance {

struct Embedder::Impl {
    EmbedderIdentity identity;
    ov::genai::Tokenizer tokenizer;
    ov::genai::TextEmbeddingPipeline pipeline;

    Impl(EmbedderIdentity id, const std::filesystem::path& dir,
         const ov::genai::TextEmbeddingPipeline::Config& config)
        : identity(std::move(id)), tokenizer(dir), pipeline(dir, "CPU", config) {}

    std::vector<float> Vector(const std::string& text) {
        return std::get<std::vector<float>>(pipeline.embed_query(text));
    }
};

namespace {

std::size_t TokenCount(ov::genai::Tokenizer& tokenizer, const std::string& text, bool truncate) {
    ov::AnyMap params{{"add_special_tokens", true}, {"truncation", truncate}};
    if (truncate) params["max_length"] = static_cast<size_t>(kEmbedMaxTokens);
    return tokenizer.encode(text, params).input_ids.get_shape()[1];
}

bool UnitLength(const std::vector<float>& v) {
    double norm = 0;
    for (float x : v) norm += static_cast<double>(x) * x;
    return std::fabs(norm - 1.0) < 1e-3;
}

void Fail(const std::string& id, const std::string& why) {
    throw std::runtime_error("guidance embedder " + id + ": " + why);
}

}  // namespace

std::unique_ptr<Embedder> Embedder::Load(const models::ModelStore& store) {
    const models::ModelInfo& info = store.Resolve("embedding", "default");
    store.Verify(info);
    if (info.device != "CPU") Fail(info.id, "manifest device is " + info.device + ", expected CPU");
    const auto weights = info.file_hashes.find("openvino_model.bin");
    if (weights == info.file_hashes.end())
        Fail(info.id, "manifest has no hash for openvino_model.bin");

    EmbedderIdentity identity;
    identity.id = info.id;
    identity.rev = weights->second;
    identity.max_tokens = kEmbedMaxTokens;

    ov::genai::TextEmbeddingPipeline::Config config;
    config.pooling_type = ov::genai::TextEmbeddingPipeline::PoolingType::MEAN;
    config.normalize = true;
    config.max_length = static_cast<size_t>(kEmbedMaxTokens);
    auto impl = std::make_unique<Impl>(std::move(identity), info.dir, config);

    // Startup checks that the tokenizer honours max_length (an IR without truncation state silently
    // ignores it) and that an over-length text still embeds to a unit vector, so the model never
    // exceeds its positions
    std::string long_text;
    for (int i = 0; i < 2000; ++i) long_text += "word ";
    if (TokenCount(impl->tokenizer, long_text, true) != static_cast<std::size_t>(kEmbedMaxTokens)) {
        Fail(info.id, "tokenizer ignores max_length");
    }
    const auto probe = impl->Vector("probe");
    if (probe.empty() || !UnitLength(probe)) Fail(info.id, "probe embedding is not a unit vector");
    const auto over = impl->Vector(long_text);
    if (over.size() != probe.size() || !UnitLength(over)) {
        Fail(info.id, "over-length text does not embed");
    }
    impl->identity.dim = static_cast<int>(probe.size());
    return std::unique_ptr<Embedder>(new Embedder(std::move(impl)));
}

Embedder::Embedder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Embedder::~Embedder() = default;

const EmbedderIdentity& Embedder::Identity() const {
    return impl_->identity;
}

Embedding Embedder::Embed(const std::string& text) {
    Embedding out;
    out.tokens = TokenCount(impl_->tokenizer, text, false);
    out.truncated = out.tokens > static_cast<std::size_t>(kEmbedMaxTokens);
    out.vector = impl_->Vector(text);
    return out;
}

}  // namespace clinicavt::guidance
