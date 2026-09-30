#include "adapters/models/model_store.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

#include "adapters/system/exe_paths.hpp"
#include "core/common/strings.hpp"

namespace clinicavt::models {

namespace {

constexpr std::int64_t kManifestVersion = 1;

[[noreturn]] void Broken(const std::filesystem::path& dir, const std::string& why) {
    throw std::runtime_error("model manifest " + dir.string() + ": " + why);
}

ModelInfo ParseManifest(const std::filesystem::path& dir) {
    std::ifstream in(dir / "manifest.json");
    nlohmann::json manifest;
    try {
        manifest = nlohmann::json::parse(in);
    } catch (const nlohmann::json::exception& e) {
        Broken(dir, e.what());
    }

    const std::int64_t version = manifest.value("manifestVersion", std::int64_t{0});
    if (version > kManifestVersion) Broken(dir, "manifest is newer than this build");
    if (version < 1) Broken(dir, "manifestVersion missing or invalid");

    ModelInfo info;
    info.dir = dir;
    try {
        info.id = manifest.at("id").get<std::string>();
        info.name = manifest.value("name", info.id);
        info.task = manifest.at("task").get<std::string>();
        info.tier = manifest.at("tier").get<std::string>();
        info.licence = manifest.at("licence").get<std::string>();
        const auto& runtime = manifest.at("runtime");
        info.device = runtime.at("device").get<std::string>();
        // Optional, defaulting to "llm" with no properties
        info.pipeline = runtime.value("pipeline", "llm");
        info.properties = runtime.value("properties", nlohmann::json::object());
        for (const auto& [name, hash] : manifest.at("files").items()) {
            info.file_hashes[name] = strings::Lower(hash.get<std::string>());
        }
        // Optional. Without sizes the load check is presence only. items() needs a named json,
        // since on a temporary it dangles
        const nlohmann::json sizes = manifest.value("bytes", nlohmann::json::object());
        for (const auto& [name, bytes] : sizes.items()) {
            info.file_bytes[name] = bytes.get<std::uintmax_t>();
        }
    } catch (const nlohmann::json::exception& e) {
        Broken(dir, e.what());
    }
    if (info.file_hashes.empty()) Broken(dir, "no files listed");
    // Unknown pipeline type counts as a corrupt manifest
    if (info.pipeline != "llm" && info.pipeline != "vlm" && info.pipeline != "embedding") {
        Broken(dir, "unknown pipeline: " + info.pipeline);
    }
    if (!info.properties.is_object()) Broken(dir, "runtime.properties must be an object");
    for (const auto& [key, value] : info.properties.items()) {
        if (!value.is_primitive() || value.is_null()) {
            Broken(dir, "runtime.properties." + key + " must be a number, string or bool");
        }
    }
    return info;
}

}  // namespace

ModelStore::ModelStore(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) return;  // valid empty store
    // OpenVINO keys its compile cache on the path string, so each model
    // needs one spelling
    std::error_code error;
    auto canonical = std::filesystem::canonical(root, error);
    if (error) canonical = root;
    const auto cache_root = system::LocalDataRoot() / "cache";
    for (const auto& entry : std::filesystem::directory_iterator(canonical)) {
        if (!entry.is_directory()) continue;
        if (!std::filesystem::exists(entry.path() / "manifest.json")) continue;
        auto& info = models_.emplace_back(ParseManifest(entry.path()));
        info.cache_dir = cache_root / info.id;
    }
    std::sort(models_.begin(), models_.end(),
              [](const ModelInfo& a, const ModelInfo& b) { return a.id < b.id; });
}

const ModelInfo& ModelStore::Resolve(std::string_view task, std::string_view tier) const {
    const ModelInfo* found = nullptr;
    for (const auto& model : models_) {
        if (model.task != task || model.tier != tier) continue;
        if (found != nullptr) {
            throw std::runtime_error("both " + found->id + " and " + model.id + " claim " +
                                     std::string(task) + "/" + std::string(tier));
        }
        found = &model;
    }
    if (found == nullptr) {
        std::string installed;
        for (const auto& model : models_) {
            installed += " " + model.id + "(" + model.task + "/" + model.tier + ")";
        }
        throw std::runtime_error("no model for " + std::string(task) + "/" + std::string(tier) +
                                 "; installed:" + (installed.empty() ? " none" : installed));
    }
    return *found;
}

void ModelStore::Verify(const ModelInfo& model) const {
    // A dropped drive loses the whole folder, so a missing folder is reported before any file
    std::error_code error;
    if (!std::filesystem::is_directory(model.dir, error)) {
        throw std::runtime_error(model.id + ": the models folder cannot be read (" +
                                 model.dir.string() + ")");
    }
    for (const auto& [name, expected] : model.file_hashes) {
        const std::filesystem::path path = model.dir / name;
        if (!std::filesystem::exists(path)) {
            throw std::runtime_error(model.id + ": missing file " + name);
        }
        const auto bytes = model.file_bytes.find(name);
        if (bytes != model.file_bytes.end() && std::filesystem::file_size(path) != bytes->second) {
            throw std::runtime_error(model.id + ": " + name + " is " +
                                     std::to_string(std::filesystem::file_size(path)) +
                                     " bytes, the manifest says " + std::to_string(bytes->second));
        }
    }
}

}  // namespace clinicavt::models
