#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <random>
#include <span>
#include <stdexcept>
#include <string>

#include "adapters/storage/chunk_cipher.hpp"
#include "ports/session_store.hpp"

// Row helpers shared by the files that implement SqliteSessionStore
namespace clinicavt::store::rows {

// Max audio buffered in memory while commits fail. Older frames are dropped and counted as lost
inline constexpr std::chrono::seconds kPendingBound(30);
inline constexpr std::int64_t kSqlitePageLimit = 1073741823;  // the default max_page_count

struct KindSpec {
    const char* name;  // documents.kind
    Domain domain;     // sealing domain
};

inline KindSpec SpecFor(DocumentKind kind) {
    switch (kind) {
        case DocumentKind::kNote:
            return {"note", Domain::kNote};
        case DocumentKind::kPatient:
            return {"patient", Domain::kPatient};
        case DocumentKind::kTranslation:
            return {"translation", Domain::kTranslation};
        case DocumentKind::kLabel:
            return {"label", Domain::kLabel};
        case DocumentKind::kSummary:
            return {"summary", Domain::kSummary};
        case DocumentKind::kReflection:
            return {"reflection", Domain::kReflection};
        case DocumentKind::kGuidance:
            return {"guidance", Domain::kGuidance};
    }
    throw std::invalid_argument("unknown document kind");
}

inline std::string RandomId() {
    std::random_device device;
    std::string id;
    for (int i = 0; i < 4; ++i) id += std::format("{:08x}", device());
    return id;
}

// New slots start at a random sequence below 2^62 so a deleted and rewritten
// slot never reuses an IV
inline std::int64_t FreshSlotSequence() {
    std::random_device device;
    const std::uint64_t high = device();
    const std::uint64_t low = device();
    return static_cast<std::int64_t>(((high << 32 | low) >> 2) | 1);
}

inline std::span<const std::uint8_t> AsBytes(std::span<const float> frames) {
    return {reinterpret_cast<const std::uint8_t*>(frames.data()), frames.size_bytes()};
}

inline std::span<const std::uint8_t> AsBytes(const std::string& text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

}  // namespace clinicavt::store::rows
