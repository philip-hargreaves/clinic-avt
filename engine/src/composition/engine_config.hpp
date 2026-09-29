#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace clinicavt::composition {

// The engine's command line takes flags first, then the positional pipe name, store root, models
// root and replay wav
struct EngineConfig {
    std::wstring pipe_name;
    std::filesystem::path store_root;
    std::filesystem::path models_root;
    std::filesystem::path corpora_root;
    std::string guidelines_override;  // empty means the Documents folder
    std::string asr_device;
    std::string note_tier;
    std::string replay_wav;  // forces every session to replay it
    bool include_research = false;
    bool scripted = false;
    bool allow_replay = false;
};

// Arguments are UTF-8. Throws std::runtime_error for a combination the engine refuses
EngineConfig ParseConfig(std::vector<std::string> args);

}  // namespace clinicavt::composition
