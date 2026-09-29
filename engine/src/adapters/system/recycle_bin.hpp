#pragma once

#include <filesystem>

namespace clinicavt::system {

// Moves a file to the Recycle Bin so a removed guideline can be restored.
// Throws a store error on failure
void RecycleFile(const std::filesystem::path& path);

}  // namespace clinicavt::system
