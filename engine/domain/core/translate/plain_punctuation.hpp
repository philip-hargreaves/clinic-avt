#pragma once

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace clinicavt::translate {

// NLLB's vocabulary has none of these characters, so each would become an
// unknown token
inline std::string PlainPunctuation(std::string_view text) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kPlain{{
        {"\xE2\x80\x98", "'"},
        {"\xE2\x80\x99", "'"},
        {"\xE2\x80\x9C", "\""},
        {"\xE2\x80\x9D", "\""},
        {"\xE2\x80\x93", "-"},
        {"\xE2\x80\x94", "-"},
        {"\xC2\xA0", " "},
        {"\xE2\x80\xA6", "..."},
    }};
    std::string out;
    out.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        bool replaced = false;
        for (const auto& [from, to] : kPlain) {
            if (text.substr(at, from.size()) == from) {
                out += to;
                at += from.size();
                replaced = true;
                break;
            }
        }
        if (!replaced) out += text[at++];
    }
    return out;
}

}  // namespace clinicavt::translate
