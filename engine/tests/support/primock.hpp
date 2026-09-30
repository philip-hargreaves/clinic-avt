#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace clinicavt::test {

// A file of the PriMock consultation day1_consultation01, which is not in the repo. The tests
// that read it skip when it is missing
struct PrimockFile {
    const char* variable;  // overrides the default path when set
    const char* fallback;
};

inline constexpr PrimockFile kPrimockMixed{
    "CLINICAVT_PRIMOCK_MIXED",
    "C:/dev/intelliscribe/bench/transcription/mixed/day1_consultation01_mixed.wav"};
inline constexpr PrimockFile kPrimockReference{
    "CLINICAVT_PRIMOCK_REFERENCE",
    "C:/dev/intelliscribe/bench/transcription/references/day1_consultation01.json"};

inline std::string PrimockLocation(const PrimockFile& file) {
#pragma warning(suppress : 4996)
    const char* value = std::getenv(file.variable);
    return value != nullptr && *value != '\0' ? value : file.fallback;
}

// Empty when the file is missing
inline std::string PrimockPath(const PrimockFile& file) {
    std::string path = PrimockLocation(file);
    return std::filesystem::exists(path) ? path : "";
}

inline std::string PrimockSkipReason(const PrimockFile& file) {
    return "PriMock file not found at " + PrimockLocation(file) + ". Set " + file.variable +
           " to its location to run this";
}

}  // namespace clinicavt::test
