#pragma once

#include <filesystem>
#include <string>

namespace clinicavt::composition {

// Returns the guidelines folder. A new default folder gets the guidelines shipped beside the
// models
std::filesystem::path PrepareGuidelinesFolder(const std::string& override,
                                              const std::filesystem::path& models_root);

}  // namespace clinicavt::composition
