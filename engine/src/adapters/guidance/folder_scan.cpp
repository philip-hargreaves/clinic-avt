#include "adapters/guidance/folder_scan.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <system_error>

#include "core/common/strings.hpp"
#include "core/common/utf8.hpp"

namespace clinicavt::guidance {
namespace {

std::string LowerExtension(const std::filesystem::path& path) {
    return strings::Lower(path.extension().string());
}

bool HiddenOrSystem(const std::filesystem::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
}

// Copies in progress, Office locks and downloads in the making
bool Transient(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    const auto ext = LowerExtension(path);
    return name.starts_with("~") || name.starts_with(".") || ext == ".tmp" ||
           ext == ".crdownload" || ext == ".partial";
}

}  // namespace

std::string Mime(const std::filesystem::path& path) {
    const auto ext = LowerExtension(path);
    if (ext == ".txt") return "text/plain";
    if (ext == ".md" || ext == ".markdown") return "text/markdown";
    if (ext == ".pdf") return kPdf;
    return "";
}

std::int64_t Ticks(std::filesystem::file_time_type time) {
    return time.time_since_epoch().count();
}

bool Unlocked(const std::filesystem::path& path) {
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    CloseHandle(handle);
    return true;
}

FolderListing ListFolder(const std::filesystem::path& folder, int max_depth,
                         const std::function<bool(const std::string& mime)>& supported,
                         const std::string& skip) {
    FolderListing listing;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        folder, std::filesystem::directory_options::skip_permission_denied, ec);
    for (const std::filesystem::recursive_directory_iterator end; !ec && it != end;
         it.increment(ec)) {
        const auto& entry = *it;
        const bool symlink = entry.is_symlink(ec);
        if (it.depth() >= max_depth || symlink) it.disable_recursion_pending();
        if (symlink || !entry.is_regular_file(ec)) continue;
        if (HiddenOrSystem(entry.path()) || Transient(entry.path())) continue;
        const auto relative = utf8::FromPath(entry.path().lexically_relative(folder));
        if (relative == skip) continue;
        if (!supported(Mime(entry.path()))) {
            ++listing.unsupported;
            continue;
        }
        listing.files.push_back({relative, static_cast<std::int64_t>(entry.file_size(ec)),
                                 Ticks(entry.last_write_time(ec))});
    }
    return listing;
}

}  // namespace clinicavt::guidance
