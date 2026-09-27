#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace clinicavt::guidance {

inline constexpr const char* kPdf = "application/pdf";

// text/plain, text/markdown or kPdf by extension, else empty
std::string Mime(const std::filesystem::path& path);

// The open does not share writing, so a file another program is still writing
// refuses it
bool Unlocked(const std::filesystem::path& path);

std::int64_t Ticks(std::filesystem::file_time_type time);

// A file the guidelines folder holds, keyed as the index keys it
struct FolderFile {
    std::string path;  // relative to the folder, UTF-8
    std::int64_t size = 0;
    std::int64_t modified = 0;
};

struct FolderListing {
    std::vector<FolderFile> files;
    int unsupported = 0;
};

// The regular files under `folder` to `max_depth`, symlinks not followed.
// Hidden, system and transient files and the one named `skip` are left out.
// Files whose type `supported` refuses are counted
FolderListing ListFolder(const std::filesystem::path& folder, int max_depth,
                         const std::function<bool(const std::string& mime)>& supported,
                         const std::string& skip);

}  // namespace clinicavt::guidance
