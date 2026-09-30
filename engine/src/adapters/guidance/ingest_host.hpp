#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/guidance/page_text.hpp"

namespace clinicavt::guidance {

struct HostLimits {
    std::chrono::milliseconds timeout{180'000};
    std::size_t output_cap = 64u << 20;
    std::size_t memory_cap = 1u << 30;
};

// Why a run gave nothing back, one of cannotOpen, password, outputBound, badPage, timeout,
// crashed or badOutput
class HostError : public std::runtime_error {
   public:
    HostError(std::string reason, const std::string& what)
        : std::runtime_error(what), reason_(std::move(reason)) {}

    const std::string& Reason() const {
        return reason_;
    }

   private:
    std::string reason_;
};

// A rendered page as a 32-bit top-down BMP
struct Bitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bmp;
};

// Runs clinicavt_ingest_host once per call in its own job (one process, a memory cap, killed
// with the engine). The document goes in on stdin and the pages or pixels come back on stdout,
// validated before use
class IngestHost {
   public:
    explicit IngestHost(std::filesystem::path exe, HostLimits limits = {});

    std::vector<Page> Extract(std::span<const std::uint8_t> document) const;
    Bitmap Render(std::span<const std::uint8_t> document, int page, int dpi) const;

   private:
    std::string Run(std::span<const std::uint8_t> document, const std::wstring& args) const;

    std::filesystem::path exe_;
    HostLimits limits_;
};

}  // namespace clinicavt::guidance
