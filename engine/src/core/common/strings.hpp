#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ASCII text helpers shared by the stages. Bytes above 127 pass through untouched
namespace clinicavt::strings {

inline std::string Lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline bool Contains(std::string_view s, std::string_view needle) {
    return s.find(needle) != std::string_view::npos;
}

inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// Whitespace-delimited words, as typed, viewing s
inline std::vector<std::string_view> Words(std::string_view s) {
    std::vector<std::string_view> words;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
        const std::size_t start = i;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) == 0) ++i;
        if (i > start) words.push_back(s.substr(start, i - start));
    }
    return words;
}

// Lower-cased runs of the bytes word_char accepts; everything else separates
inline std::vector<std::string> LowerTokens(std::string_view s, bool (*word_char)(unsigned char)) {
    std::vector<std::string> tokens;
    std::string token;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (word_char(u)) {
            token.push_back(static_cast<char>(std::tolower(u)));
        } else if (!token.empty()) {
            tokens.push_back(std::move(token));
            token.clear();
        }
    }
    if (!token.empty()) tokens.push_back(std::move(token));
    return tokens;
}

// Whitespace runs to one space, none at either end
inline std::string Squeeze(std::string_view s) {
    std::string out;
    bool space = true;
    for (const unsigned char c : s) {
        if (std::isspace(c) != 0) {
            if (!space) out.push_back(' ');
            space = true;
        } else {
            out.push_back(static_cast<char>(c));
            space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

inline int WordCount(std::string_view s) {
    int words = 0;
    bool in_word = false;
    for (const unsigned char c : s) {
        const bool space = std::isspace(c) != 0;
        if (!space && !in_word) ++words;
        in_word = !space;
    }
    return words;
}

// CRLF and a lone CR to LF. A WinUI text box ends its lines with CR
inline std::string UnixLines(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\r') {
            out.push_back(s[i]);
        } else if (i + 1 >= s.size() || s[i + 1] != '\n') {
            out.push_back('\n');
        }
    }
    return out;
}

// A full stop, question or exclamation mark once trailing spaces, closing
// quotes and brackets and any non-ASCII closer are set aside
inline bool EndsSentence(std::string_view s) {
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        const auto c = static_cast<unsigned char>(*it);
        if (std::isspace(c) != 0 || c > 127) continue;
        if (*it == '"' || *it == '\'' || *it == ')') continue;
        return *it == '.' || *it == '?' || *it == '!';
    }
    return false;
}

}  // namespace clinicavt::strings
