#include "adapters/system/exe_paths.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdlib>
#include <stdexcept>

namespace clinicavt::system {

std::filesystem::path ExeDir() {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    return std::filesystem::path(exe_path).parent_path();
}

std::filesystem::path DefaultModelsRoot() {
    const auto exe_dir = ExeDir();
    auto root = exe_dir / "models";
    if (!std::filesystem::exists(root)) root = exe_dir.parent_path() / "models";
    return root;
}

std::filesystem::path LocalDataRoot() {
    wchar_t* local_app_data = nullptr;
    if (_wdupenv_s(&local_app_data, nullptr, L"LOCALAPPDATA") != 0 || local_app_data == nullptr) {
        throw std::runtime_error("LOCALAPPDATA is not set");
    }
    const auto root = std::filesystem::path(local_app_data) / "ClinicAVT";
    std::free(local_app_data);
    return root;
}

}  // namespace clinicavt::system
