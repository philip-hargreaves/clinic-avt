#pragma once

#include <filesystem>

namespace clinicavt::system {

// Helpers next to the engine; the orphan scan and spawn must use these names
inline constexpr const wchar_t* kNoteHostExe = L"clinicavt_note_host.exe";
inline constexpr const wchar_t* kIngestHostExe = L"clinicavt_ingest_host.exe";

std::filesystem::path ExeDir();

// Production: next to the executable (package dir). Packaged debug runs sit one level below
std::filesystem::path DefaultModelsRoot();

}  // namespace clinicavt::system
