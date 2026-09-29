#include "composition/guidelines_setup.hpp"

#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <shlobj.h>
// clang-format on

#include "adapters/guidance/document_ingest.hpp"
#include "core/common/log.hpp"
#include "core/common/utf8.hpp"

namespace clinicavt::composition {

namespace {

// Default: the user's Documents folder, unless overridden
std::filesystem::path GuidelinesFolder(const std::string& override) {
    if (!override.empty()) return utf8::ToPath(override);
    PWSTR documents = nullptr;
    std::filesystem::path folder;
    if (SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents) == S_OK) {
        folder = std::filesystem::path(documents) / "ClinicAVT guidelines";
    }
    CoTaskMemFree(documents);
    if (folder.empty()) throw std::runtime_error("no Documents folder and no --guidelines given");
    return folder;
}

}  // namespace

std::filesystem::path PrepareGuidelinesFolder(const std::string& override,
                                              const std::filesystem::path& models_root) {
    const auto guidelines = GuidelinesFolder(override);
    if (override.empty()) {
        const auto seeded =
            guidance::SeedGuidelines(guidelines, models_root.parent_path() / "guidelines");
        if (seeded > 0) {
            log::Printf("clinicavt-engine: guidelines folder started with %zu documents\n", seeded);
        }
    }
    return guidelines;
}

}  // namespace clinicavt::composition
