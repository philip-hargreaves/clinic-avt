#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace clinicavt::system {

// SHA-256 as lowercase hex, over bytes in memory or a file streamed from disk
std::string Sha256Hex(std::span<const std::uint8_t> bytes);
std::string Sha256File(const std::filesystem::path& path);

}  // namespace clinicavt::system
