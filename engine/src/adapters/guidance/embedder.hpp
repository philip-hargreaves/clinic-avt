#pragma once

#include <memory>
#include <string>

#include "adapters/interfaces/embedder.hpp"

namespace clinicavt::models {
class ModelStore;
}  // namespace clinicavt::models

namespace clinicavt::guidance {

inline constexpr int kEmbedMaxTokens = 512;

// Staged embedding model on CPU through GenAI, with mean pooling, normalisation and no instruction
// prefix. Load verifies files, runs the startup checks and throws with the failure
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
