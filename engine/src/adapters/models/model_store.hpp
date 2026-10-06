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
    std::filesystem::path cache_dir;                   // OpenVINO compile cache, empty for none
    std::map<std::string, std::string> file_hashes;    // filename -> sha256 hex, for provenance
    std::map<std::string, std::uintmax_t> file_bytes;  // filename -> size, when the manifest says
};

// Per-model manifest.json dirs under one root. Parsing fails closed. A load checks presence and
// size only, as integrity is checked at delivery (fetch, staging, package signature). Hashes are
// kept for provenance, and the embedder's weight hash is also its revision. Each model compiles
// into LocalDataRoot()/cache/<id>. Has no OpenVINO dependency
class ModelStore {
   public:
    explicit ModelStore(const std::filesystem::path& root);

    const std::vector<ModelInfo>& List() const {
        return models_;
    }

    // The model for a role. Throws if none or several match
    const ModelInfo& Resolve(std::string_view task, std::string_view tier) const;

    // Throws naming the first file missing or of the wrong size. Reads no bytes
    void Verify(const ModelInfo& model) const;

   private:
    std::vector<ModelInfo> models_;
};

// The model folder is read-only once installed, so the cache is per user
inline std::filesystem::path CacheDir(const ModelInfo& info) {
    return info.cache_dir;
}

// True once compiled on this machine (cache exists)
inline bool Compiled(const ModelInfo& info) {
    return !info.cache_dir.empty() && std::filesystem::exists(info.cache_dir);
}

}  // namespace clinicavt::models
