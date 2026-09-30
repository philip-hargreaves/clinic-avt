#include "adapters/archive/archive_password.hpp"

#include "ports/session_archive.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace clinicavt::archive {

namespace {

// Only reached for invalid UTF-8, which pipe JSON never contains
[[noreturn]] void Unusable() {
    throw ArchiveError(ArchiveCode::kWeakPassword);
}

std::wstring Wide(std::string_view utf8) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) Unusable();
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), size);
    return wide;
}

std::wstring Nfc(const std::wstring& wide) {
    const int length = static_cast<int>(wide.size());
    int size = NormalizeString(NormalizationC, wide.data(), length, nullptr, 0);
    // The first size is an estimate; a short buffer returns a better one, negated
    for (int attempt = 0; attempt < 8 && size > 0; ++attempt) {
        std::wstring normal(static_cast<std::size_t>(size), L'\0');
        const int got = NormalizeString(NormalizationC, wide.data(), length, normal.data(), size);
        if (got > 0) {
            normal.resize(static_cast<std::size_t>(got));
            return normal;
        }
        const DWORD error = GetLastError();
        Wipe(normal);
        if (error != ERROR_INSUFFICIENT_BUFFER) break;
        size = -got;
    }
    Unusable();
}

std::string Utf8(const std::wstring& wide) {
    const int size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) Unusable();
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), size, nullptr, nullptr);
    return utf8;
}

}  // namespace

std::string KdfInput(std::string_view password) {
    if (password.empty()) return {};
    std::wstring wide = Wide(password);
    WipeOnExit wipe_wide{wide};
    std::wstring normal = Nfc(wide);
    WipeOnExit wipe_normal{normal};
    return Utf8(normal);
}

std::size_t CodePoints(std::string_view utf8) {
    std::size_t count = 0;
    for (const char c : utf8) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++count;
    }
    return count;
}

void Wipe(std::string& secret) {
    SecureZeroMemory(secret.data(), secret.size());
    secret.clear();
}

void Wipe(std::wstring& secret) {
    SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));
    secret.clear();
}

}  // namespace clinicavt::archive
