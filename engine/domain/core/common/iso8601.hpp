#pragma once

#include <chrono>
#include <format>
#include <string>

namespace clinicavt {

// UTC to the second. The stores compare and order these as text, so every writer uses this form
inline std::string Iso8601(std::chrono::sys_seconds at) {
    return std::format("{:%FT%T}Z", at);
}

inline std::string Iso8601Now() {
    return Iso8601(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

}  // namespace clinicavt
