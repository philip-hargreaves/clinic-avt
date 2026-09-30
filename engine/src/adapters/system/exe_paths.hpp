#pragma once

#include <filesystem>

namespace clinicavt::system {

// Helper executables next to the engine. The orphan scan and spawn must use these names
inline constexpr const wchar_t* kNoteHostExe = L"clinicavt_note_host.exe";
inline constexpr const wchar_t* kIngestHostExe = L"clinicavt_ingest_host.exe";

std::filesystem::path ExeDir();

// In production the models sit next to the executable, in the package dir. Packaged debug runs sit
// one level below
std::filesystem::path DefaultModelsRoot();

// %LOCALAPPDATA%\ClinicAVT, for the store and the compile cache. Throws when LOCALAPPDATA is unset
std::filesystem::path LocalDataRoot();

}  // namespace clinicavt::system
