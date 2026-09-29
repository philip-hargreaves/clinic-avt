#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace clinicavt::guidance {

// Staged embedder identity; a corpus must match it
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

// Shared by the indexer and the retriever. One text at a time
class IEmbedder {
   public:
    virtual ~IEmbedder() = default;
    virtual const EmbedderIdentity& Identity() const = 0;
    virtual Embedding Embed(const std::string& text) = 0;
};

}  // namespace clinicavt::guidance
