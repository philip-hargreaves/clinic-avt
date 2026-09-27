#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace clinicavt::models {
class ModelStore;
}  // namespace clinicavt::models

namespace clinicavt::guidance {

// The staged embedder a corpus must have been built with
struct EmbedderIdentity {
    std::string id;
    std::string rev;  // sha256 of the weights
    int dim = 0;
    int max_tokens = 0;

    bool operator==(const EmbedderIdentity&) const = default;
};

struct Embedding {
    std::vector<float> vector;  // unit length, Identity().dim floats
    std::size_t tokens = 0;     // before truncation
    bool truncated = false;     // the text ran past max_tokens
};

// The seam the indexer and the retriever share. One text at a time
class IEmbedder {
   public:
    virtual ~IEmbedder() = default;
    virtual const EmbedderIdentity& Identity() const = 0;
    virtual Embedding Embed(const std::string& text) = 0;
};

inline constexpr int kEmbedMaxTokens = 512;

// The staged embedding model on the CPU through the GenAI pipeline: mean
// pooling, normalised, no instruction strings. Load verifies the files and
// runs the startup guards. It throws naming the failure
class Embedder : public IEmbedder {
   public:
    static std::unique_ptr<Embedder> Load(const models::ModelStore& store);
    ~Embedder() override;

    const EmbedderIdentity& Identity() const override;
    Embedding Embed(const std::string& text) override;

   private:
    struct Impl;
    explicit Embedder(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace clinicavt::guidance
