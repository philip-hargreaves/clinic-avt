#include "composition/engine_config.hpp"

#include <cstdlib>
#include <stdexcept>

#include "adapters/system/exe_paths.hpp"
#include "core/common/cli_args.hpp"
#include "core/common/utf8.hpp"

namespace clinicavt::composition {

namespace {

std::filesystem::path StoreRoot(const std::vector<std::string>& args) {
    if (args.size() > 1) return utf8::ToPath(args[1]);
    char* local_app_data = nullptr;
    if (_dupenv_s(&local_app_data, nullptr, "LOCALAPPDATA") != 0 || local_app_data == nullptr) {
        throw std::runtime_error("LOCALAPPDATA is not set and no store root was given");
    }
    const auto root = std::filesystem::path(local_app_data) / "ClinicAVT" / "store";
    std::free(local_app_data);
    return root;
}

}  // namespace

EngineConfig ParseConfig(std::vector<std::string> args) {
    EngineConfig config;
    config.asr_device = TakeFlag(args, "--asr-device");
    config.note_tier = TakeFlag(args, "--note-tier");
    const std::string corpora_override = TakeFlag(args, "--corpora");
    config.guidelines_override = TakeFlag(args, "--guidelines");
    // Dev builds also search demo corpora marked research
    config.include_research = TakeSwitch(args, "--include-research");
    // CI and tests run without models, so a role that is not installed gets a stand-in
    // and consultations still run
    config.scripted = TakeSwitch(args, "--scripted");
    // Tests and evaluation play wav files as the microphone. The shipped app never does
    config.allow_replay = TakeSwitch(args, "--allow-replay");
    if (args.size() > 3 && !config.allow_replay) {
        throw std::runtime_error("a replay wav needs --allow-replay");
    }
    config.pipe_name = L"\\\\.\\pipe\\LOCAL\\clinicavt-engine";
    if (args.size() > 0) {
        config.pipe_name = L"\\\\.\\pipe\\" + utf8::ToPath(args[0]).wstring();
    }
    config.store_root = StoreRoot(args);
    config.models_root = args.size() > 2 ? utf8::ToPath(args[2]) : system::DefaultModelsRoot();
    // Corpora live next to the models. Each is replaced as a whole directory
    config.corpora_root = corpora_override.empty() ? config.models_root.parent_path() / "corpora"
                                                   : utf8::ToPath(corpora_override);
    if (args.size() > 3) config.replay_wav = args[3];
    return config;
}

}  // namespace clinicavt::composition
