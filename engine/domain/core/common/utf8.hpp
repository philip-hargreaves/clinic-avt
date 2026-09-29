#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace clinicavt::utf8 {

// One code point from `at`, U+FFFD for a broken sequence
inline char32_t Decode(std::string_view s, std::size_t& at) {
    const auto lead = static_cast<unsigned char>(s[at]);
    const int extra = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
    if ((lead >= 0x80 && extra == 0) || at + extra >= s.size()) {
        at = std::min(s.size(), at + 1 + extra);
        return 0xFFFD;
    }
    char32_t cp = extra == 0 ? lead : lead & (0x3F >> extra);
    for (int i = 1; i <= extra; ++i) {
        const auto c = static_cast<unsigned char>(s[at + i]);
        if ((c & 0xC0) != 0x80) {
            at += i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (c & 0x3F);
    }
    at += extra + 1;
    return cp;
}

inline std::vector<char32_t> CodePoints(std::string_view s) {
    std::vector<char32_t> out;
    for (std::size_t at = 0; at < s.size();) out.push_back(Decode(s, at));
    return out;
}

inline void Encode(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

inline std::string FromPath(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

inline std::filesystem::path ToPath(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

}  // namespace clinicavt::utf8
