#include "adapters/storage/chunk_cipher.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

#include "ports/store_error.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>
// clang-format on

namespace clinicavt::store {

namespace {

constexpr std::size_t kKeyBytes = 32;
constexpr std::size_t kIvBytes = 12;
constexpr std::size_t kTagBytes = 16;

void Check(NTSTATUS status, const char* what) {
    if (status < 0) {
        throw StoreError(StoreCode::kOther, std::string(what) + " failed, NTSTATUS " +
                                                std::to_string(static_cast<long>(status)));
    }
}

void PutSeq(std::uint8_t* out, std::uint64_t seq) {
    for (int i = 7; i >= 0; --i) {
        out[i] = static_cast<std::uint8_t>(seq & 0xFF);
        seq >>= 8;
    }
}

}  // namespace

struct ChunkCipher::Impl {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    std::uint8_t key_bytes[kKeyBytes] = {};

    Impl() = default;

    explicit Impl(std::span<const std::uint8_t> key_material) {
        if (key_material.size() != kKeyBytes) throw StoreError(StoreCode::kOther, "bad key length");
        std::memcpy(key_bytes, key_material.data(), kKeyBytes);
        Load();
    }

    // Makes the AES key from key_bytes
    void Load() {
        Check(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0),
              "BCryptOpenAlgorithmProvider");
        Check(
            BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0),
            "BCryptSetProperty");
        Check(BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, key_bytes,
                                         static_cast<ULONG>(kKeyBytes), 0),
              "BCryptGenerateSymmetricKey");
    }

    ~Impl() {
        if (key != nullptr) BCryptDestroyKey(key);
        if (alg != nullptr) BCryptCloseAlgorithmProvider(alg, 0);
        SecureZeroMemory(key_bytes, kKeyBytes);
    }
};

ChunkCipher::ChunkCipher(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ChunkCipher::ChunkCipher(ChunkCipher&&) noexcept = default;
ChunkCipher& ChunkCipher::operator=(ChunkCipher&&) noexcept = default;
ChunkCipher::~ChunkCipher() = default;

ChunkCipher ChunkCipher::Generate() {
    std::uint8_t key_material[kKeyBytes];
    Check(BCryptGenRandom(nullptr, key_material, static_cast<ULONG>(kKeyBytes),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG),
          "BCryptGenRandom");
    auto cipher = ChunkCipher(std::make_unique<Impl>(std::span(key_material)));
    SecureZeroMemory(key_material, kKeyBytes);
    return cipher;
}

ChunkCipher ChunkCipher::FromWrapped(std::span<const std::uint8_t> wrapped) {
    DATA_BLOB in{static_cast<DWORD>(wrapped.size()), const_cast<std::uint8_t*>(wrapped.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &out)) {
        throw StoreError(StoreCode::kAuth, "CryptUnprotectData failed");
    }
    std::unique_ptr<Impl> impl;
    try {
        impl = std::make_unique<Impl>(std::span(out.pbData, out.cbData));
    } catch (...) {
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        throw;
    }
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return ChunkCipher(std::move(impl));
}

ChunkCipher ChunkCipher::FromPassword(std::string_view password, std::span<const std::uint8_t> salt,
                                      std::uint32_t iterations) {
    // Derived into the heap Impl, never a stack copy, and zeroed by its destructor on any throw
    auto impl = std::make_unique<Impl>();
    BCRYPT_ALG_HANDLE hmac = nullptr;
    Check(BCryptOpenAlgorithmProvider(&hmac, BCRYPT_SHA256_ALGORITHM, nullptr,
                                      BCRYPT_ALG_HANDLE_HMAC_FLAG),
          "BCryptOpenAlgorithmProvider");
    const NTSTATUS status =
        BCryptDeriveKeyPBKDF2(hmac, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
                              static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt.data()),
                              static_cast<ULONG>(salt.size()), iterations, impl->key_bytes,
                              static_cast<ULONG>(kKeyBytes), 0);
    BCryptCloseAlgorithmProvider(hmac, 0);
    Check(status, "BCryptDeriveKeyPBKDF2");
    impl->Load();
    return ChunkCipher(std::move(impl));
}

std::vector<std::uint8_t> ChunkCipher::Wrapped() const {
    DATA_BLOB in{static_cast<DWORD>(kKeyBytes), const_cast<std::uint8_t*>(impl_->key_bytes)};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"ClinicAVT session key", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        throw StoreError(StoreCode::kAuth, "CryptProtectData failed");
    }
    std::vector<std::uint8_t> wrapped(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return wrapped;
}

std::vector<std::uint8_t> ChunkCipher::Seal(Domain domain, std::string_view context,
                                            std::uint64_t seq,
                                            std::span<const std::uint8_t> plain) const {
    std::uint8_t iv[kIvBytes] = {static_cast<std::uint8_t>(domain)};
    PutSeq(iv + 4, seq);
    std::vector<std::uint8_t> aad(1 + context.size() + 8);
    aad[0] = static_cast<std::uint8_t>(domain);
    std::memcpy(aad.data() + 1, context.data(), context.size());
    PutSeq(aad.data() + 1 + context.size(), seq);

    std::vector<std::uint8_t> sealed(plain.size() + kTagBytes);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = iv;
    info.cbNonce = kIvBytes;
    info.pbAuthData = aad.data();
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = sealed.data() + plain.size();
    info.cbTag = kTagBytes;

    ULONG written = 0;
    Check(BCryptEncrypt(impl_->key, const_cast<std::uint8_t*>(plain.data()),
                        static_cast<ULONG>(plain.size()), &info, nullptr, 0, sealed.data(),
                        static_cast<ULONG>(plain.size()), &written, 0),
          "BCryptEncrypt");
    return sealed;
}

std::vector<std::uint8_t> ChunkCipher::Open(Domain domain, std::string_view context,
                                            std::uint64_t seq,
                                            std::span<const std::uint8_t> sealed) const {
    if (sealed.size() < kTagBytes)
        throw StoreError(StoreCode::kAuth, "chunk failed authentication");
    const std::size_t plain_size = sealed.size() - kTagBytes;

    std::uint8_t iv[kIvBytes] = {static_cast<std::uint8_t>(domain)};
    PutSeq(iv + 4, seq);
    std::vector<std::uint8_t> aad(1 + context.size() + 8);
    aad[0] = static_cast<std::uint8_t>(domain);
    std::memcpy(aad.data() + 1, context.data(), context.size());
    PutSeq(aad.data() + 1 + context.size(), seq);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = iv;
    info.cbNonce = kIvBytes;
    info.pbAuthData = aad.data();
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = const_cast<std::uint8_t*>(sealed.data() + plain_size);
    info.cbTag = kTagBytes;

    std::vector<std::uint8_t> plain(plain_size);
    ULONG written = 0;
    const NTSTATUS status = BCryptDecrypt(impl_->key, const_cast<std::uint8_t*>(sealed.data()),
                                          static_cast<ULONG>(plain_size), &info, nullptr, 0,
                                          plain_size > 0 ? plain.data() : nullptr,
                                          static_cast<ULONG>(plain_size), &written, 0);
    if (status < 0) throw StoreError(StoreCode::kAuth, "chunk failed authentication");
    return plain;
}

}  // namespace clinicavt::store
