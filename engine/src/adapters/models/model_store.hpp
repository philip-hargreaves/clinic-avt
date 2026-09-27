#pragma once

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace clinicavt::models {

struct ModelInfo {
    std::string id;
    std::string name;  // display name, the id when the manifest has none
    // asr | vad | diarisation | segmentation | note | translation | embedding
    std::string task;
    std::string tier;           // default | accuracy | constrained
    std::string device;         // GPU | CPU | NPU
    std::string licence;        // SPDX id
    std::string pipeline;       // llm | vlm | embedding GenAI pipeline, llm when absent
    nlohmann::json properties;  // OpenVINO properties passed verbatim at compile, {} when absent
    std::filesystem::path dir;
    std::map<std::string, std::string> file_hashes;    // filename -> sha256 hex: provenance
    std::map<std::string, std::uintmax_t> file_bytes;  // filename -> size, when the manifest says
};

// Per-model manifest.json dirs under one root. Parsing fails closed. A load
// checks presence and size only. Integrity is established at delivery (fetch,
// staging, package signature). Hashes are provenance; the embedding model's
// weight hash is its revision. No OpenVINO here
class ModelStore {
   public:
    explicit ModelStore(const std::filesystem::path& root);

    const std::vector<ModelInfo>& List() const {
        return models_;
    }

    // The one model serving a role. Ambiguity and absence are loud
    const ModelInfo& Resolve(std::string_view task, std::string_view tier) const;

    // Throws naming the first file missing or of the wrong size. Reads no bytes
    void Verify(const ModelInfo& model) const;

   private:
    std::vector<ModelInfo> models_;
};

// OpenVINO's compile cache, beside the weights
inline std::filesystem::path CacheDir(const ModelInfo& info) {
    return info.dir / ".cache";
}

// True once the model has been compiled on this machine
inline bool Compiled(const ModelInfo& info) {
    return std::filesystem::exists(CacheDir(info));
}

}  // namespace clinicavt::models
