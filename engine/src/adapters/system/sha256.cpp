#include "adapters/system/sha256.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

namespace clinicavt::system {

namespace {

class Hasher {
   public:
    Hasher() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
        }
        if (BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(alg_, 0);
            throw std::runtime_error("BCryptCreateHash failed");
        }
    }
    ~Hasher() {
        BCryptDestroyHash(hash_);
        BCryptCloseAlgorithmProvider(alg_, 0);
    }
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

    void Update(const void* data, std::size_t size) {
        BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)),
                       static_cast<ULONG>(size), 0);
    }

    std::string Hex() {
        unsigned char digest[32];
        BCryptFinishHash(hash_, digest, sizeof(digest), 0);
        std::string hex;
        for (const unsigned char byte : digest) {
            constexpr char kDigits[] = "0123456789abcdef";
            hex += kDigits[byte >> 4];
            hex += kDigits[byte & 0xF];
        }
        return hex;
    }

   private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

}  // namespace

std::string Sha256Hex(std::span<const std::uint8_t> bytes) {
    Hasher hasher;
    hasher.Update(bytes.data(), bytes.size());
    return hasher.Hex();
}

std::string Sha256File(const std::filesystem::path& path) {
    Hasher hasher;
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chunk(1 << 20);
    while (in.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || in.gcount() > 0) {
        hasher.Update(chunk.data(), static_cast<std::size_t>(in.gcount()));
    }
    return hasher.Hex();
}

}  // namespace clinicavt::system
