#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace clinicavt::store {

// Each IV starts with its domain, so two streams under one key never share an IV
enum class Domain : std::uint8_t {
    kAudio = 0,
    kTurns = 1,
    kNote = 2,
    kPatient = 3,
    kTranslation = 4,
    kLabel = 5,
    kSummary = 6,
    kReflection = 7,
    kGuidance = 8,
    kArchive = 9,  // a backup file, under its own password-derived key
};

// A stored payload opens only under the number it was sealed with
static_assert(static_cast<int>(Domain::kAudio) == 0);
static_assert(static_cast<int>(Domain::kTurns) == 1);
static_assert(static_cast<int>(Domain::kNote) == 2);
static_assert(static_cast<int>(Domain::kPatient) == 3);
static_assert(static_cast<int>(Domain::kTranslation) == 4);
static_assert(static_cast<int>(Domain::kLabel) == 5);
static_assert(static_cast<int>(Domain::kSummary) == 6);
static_assert(static_cast<int>(Domain::kReflection) == 7);
static_assert(static_cast<int>(Domain::kGuidance) == 8);
static_assert(static_cast<int>(Domain::kArchive) == 9);

// AES-256-GCM, one key per session or backup. The IV is the domain and sequence, and both are
// authenticated with the context (session id or backup header)
class ChunkCipher {
   public:
    static ChunkCipher Generate();
    static ChunkCipher FromWrapped(std::span<const std::uint8_t> wrapped);

    // PBKDF2-HMAC-SHA256. The caller normalises the password
    static ChunkCipher FromPassword(std::string_view password, std::span<const std::uint8_t> salt,
                                    std::uint32_t iterations);

    // The key, DPAPI-protected for the current user, safe to persist
    std::vector<std::uint8_t> Wrapped() const;

    // Returns ciphertext followed by the 16-byte tag
    std::vector<std::uint8_t> Seal(Domain domain, std::string_view context, std::uint64_t seq,
                                   std::span<const std::uint8_t> plain) const;

    // Throws on authentication failure
    std::vector<std::uint8_t> Open(Domain domain, std::string_view context, std::uint64_t seq,
                                   std::span<const std::uint8_t> sealed) const;

    ChunkCipher(ChunkCipher&&) noexcept;
    ChunkCipher& operator=(ChunkCipher&&) noexcept;
    ~ChunkCipher();

   private:
    struct Impl;
    explicit ChunkCipher(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace clinicavt::store
