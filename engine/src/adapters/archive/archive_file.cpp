#include "adapters/archive/archive_file.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "adapters/archive/archive_password.hpp"
#include "adapters/archive/archive_record.hpp"
#include "adapters/storage/chunk_cipher.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

namespace clinicavt::archive {

namespace {

using store::ChunkCipher;
using store::Domain;

constexpr char kMagic[8] = {'C', 'A', 'V', 'T', 'B', 'A', 'K', ' '};
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHeaderBytes = 8 + 2 + 4 + kSaltBytes;
constexpr std::size_t kSaltAt = 14;
// The upper bound stops a crafted header hanging a restore; a lowered count cannot weaken a
// real file, since its key changes and every record fails
constexpr std::uint32_t kMinIterations = 1000;
constexpr std::uint32_t kMaxIterations = 10000000;
constexpr std::uint32_t kTagBytes = 16;
// Checked before allocating, as the length is read before anything is authenticated
constexpr std::uint32_t kMaxSealed = 16 * 1024 * 1024 + kTagBytes;

using Header = std::array<std::uint8_t, kHeaderBytes>;

std::string_view Context(const Header& header) {
    return {reinterpret_cast<const char*>(header.data()), header.size()};
}

std::uint32_t Le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

void PutLe(std::uint8_t* p, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::span<const std::uint8_t> Salt(const Header& header) {
    return std::span(header).subspan(kSaltAt, kSaltBytes);
}

std::uint32_t Iterations(const Header& header) {
    return Le32(header.data() + 10);
}

[[noreturn]] void Fail(ArchiveCode code) {
    throw ArchiveError(code);
}

// The derivation input, wiped as soon as the key exists; the caller's copy goes with it.
// Only a writer refuses a short password: a reader just fails to open with it
ChunkCipher Derive(std::string& password, const Header& header, bool refuse_weak) {
    WipeOnExit wipe_password{password};
    std::string input = KdfInput(password);
    WipeOnExit wipe_input{input};
    if (refuse_weak && CodePoints(input) < kMinPasswordLength) Fail(ArchiveCode::kWeakPassword);
    return ChunkCipher::FromPassword(input, Salt(header), Iterations(header));
}

std::filesystem::path PartialOf(const std::filesystem::path& target) {
    std::filesystem::path partial = target;
    partial += L".partial";
    return partial;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Reading

struct ArchiveFileSource::Impl {
    std::ifstream in;
    Header header{};
    std::optional<ChunkCipher> cipher;
    Manifest manifest;
    std::streampos first_session;
    std::size_t next = 1;  // record index
    bool broken = false;

    // The plain header, checked before any key is derived
    explicit Impl(const std::filesystem::path& path) : in(path, std::ios::binary) {
        if (!in) Fail(ArchiveCode::kReadFailed);
        if (!ReadExact(header.data(), header.size())) Fail(ArchiveCode::kNotABackup);
        if (std::memcmp(header.data(), kMagic, sizeof kMagic) != 0) Fail(ArchiveCode::kNotABackup);
        const std::uint16_t version = static_cast<std::uint16_t>(header[8] | header[9] << 8);
        if (version > kVersion) Fail(ArchiveCode::kNewerVersion);
        if (version != kVersion) Fail(ArchiveCode::kDamaged);
        const std::uint32_t iterations = Iterations(header);
        if (iterations < kMinIterations || iterations > kMaxIterations) Fail(ArchiveCode::kDamaged);
    }

    bool ReadExact(std::uint8_t* out, std::size_t size) {
        in.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size));
        return static_cast<std::size_t>(in.gcount()) == size;
    }

    // Record 0 failing authentication is what a wrong password looks like
    void ReadManifest() {
        const auto plain = ReadRecord(0, ArchiveCode::kWrongPassword);
        const json j = json::parse(plain.begin(), plain.end(), nullptr, false);
        if (j.is_discarded()) Fail(ArchiveCode::kDamaged);
        manifest = ManifestFromJson(j);
        if (manifest.version != kVersion) Fail(ArchiveCode::kDamaged);
        first_session = in.tellg();
    }

    std::vector<std::uint8_t> ReadRecord(std::uint64_t index, ArchiveCode unauthentic) {
        std::uint8_t length_bytes[4];
        if (!ReadExact(length_bytes, sizeof length_bytes)) Fail(ArchiveCode::kDamaged);
        const std::uint32_t length = Le32(length_bytes);
        if (length < kTagBytes || length > kMaxSealed) Fail(ArchiveCode::kDamaged);
        std::vector<std::uint8_t> sealed(length);
        if (!ReadExact(sealed.data(), sealed.size())) Fail(ArchiveCode::kDamaged);
        try {
            return cipher->Open(Domain::kArchive, Context(header), index, sealed);
        } catch (const std::exception&) {
            Fail(unauthentic);
        }
    }

    std::optional<store::SessionRecord> Next() {
        if (broken) Fail(ArchiveCode::kDamaged);
        try {
            if (next > manifest.consultations) {
                // More than the manifest counted, or any stray byte, is damage
                if (in.peek() != std::char_traits<char>::eof()) Fail(ArchiveCode::kDamaged);
                return std::nullopt;
            }
            const auto plain = ReadRecord(next, ArchiveCode::kDamaged);
            const json j = json::parse(plain.begin(), plain.end(), nullptr, false);
            if (j.is_discarded()) Fail(ArchiveCode::kDamaged);
            store::SessionRecord record = RecordFromJson(j);
            ++next;
            return record;
        } catch (...) {
            broken = true;
            throw;
        }
    }

    void Rewind() {
        in.clear();
        in.seekg(first_session);
        next = 1;
        broken = false;
    }
};

ArchiveFileSource::ArchiveFileSource(std::filesystem::path path, std::string password) {
    WipeOnExit wipe{password};
    impl_ = std::make_unique<Impl>(path);
    impl_->cipher.emplace(Derive(password, impl_->header, false));
    impl_->ReadManifest();
}

ArchiveFileSource::ArchiveFileSource(const std::filesystem::path& path, ChunkCipher&& cipher)
    : impl_(std::make_unique<Impl>(path)) {
    impl_->cipher.emplace(std::move(cipher));
    impl_->ReadManifest();
}

ArchiveFileSource::~ArchiveFileSource() = default;

const Manifest& ArchiveFileSource::GetManifest() const {
    return impl_->manifest;
}

std::optional<store::SessionRecord> ArchiveFileSource::Next() {
    return impl_->Next();
}

void ArchiveFileSource::Rewind() {
    impl_->Rewind();
}

// ---------------------------------------------------------------------------------------------
// Writing

struct ArchiveFileSink::Impl {
    std::filesystem::path target;
    std::filesystem::path partial;
    HANDLE file = INVALID_HANDLE_VALUE;
    Header header{};
    std::optional<ChunkCipher> cipher;
    std::size_t expected = 0;
    std::uint64_t next = 0;  // record index
    std::vector<std::string> ids;
    bool finished = true;  // nothing on disk to clean up: not yet created, committed or discarded

    ~Impl() {
        if (!finished) Discard();
    }

    void Discard() noexcept {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }
        DeleteFileW(partial.c_str());
        finished = true;
    }

    // Any failure discards the partial, and the caller sees only a fixed code
    template <typename Step>
    void Guarded(Step step) {
        if (finished) Fail(ArchiveCode::kWriteFailed);
        try {
            step();
        } catch (const ArchiveError&) {
            Discard();
            throw;
        } catch (...) {
            Discard();
            Fail(ArchiveCode::kWriteFailed);
        }
    }

    void Write(const std::uint8_t* data, std::size_t size) {
        while (size > 0) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30));
            if (!WriteFile(file, data, chunk, &written, nullptr) || written == 0) {
                Fail(ArchiveCode::kWriteFailed);
            }
            data += written;
            size -= written;
        }
    }

    void WriteRecord(const json& j) {
        const std::string plain = j.dump(-1, ' ', false, json::error_handler_t::replace);
        const auto sealed = cipher->Seal(
            Domain::kArchive, Context(header), next,
            std::span(reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size()));
        if (sealed.size() > kMaxSealed) Fail(ArchiveCode::kWriteFailed);
        std::uint8_t length[4];
        PutLe(length, static_cast<std::uint32_t>(sealed.size()), 4);
        Write(length, sizeof length);
        Write(sealed.data(), sealed.size());
        ++next;
    }

    // Reads back every record under the key it was written with, before the target is touched.
    // This proves format and authentication, not the medium: the OS cache may serve the read
    void Verify() {
        try {
            ArchiveFileSource check(partial, std::move(*cipher));
            cipher.reset();
            if (check.GetManifest().consultations != expected) Fail(ArchiveCode::kWriteFailed);
            for (const std::string& id : ids) {
                const auto record = check.Next();
                if (!record || record->id != id) Fail(ArchiveCode::kWriteFailed);
            }
            if (check.Next()) Fail(ArchiveCode::kWriteFailed);
        } catch (const ArchiveError&) {
            Fail(ArchiveCode::kWriteFailed);
        }
    }
};

ArchiveFileSink::ArchiveFileSink(std::filesystem::path target, std::string password,
                                 std::uint32_t iterations)
    : impl_(std::make_unique<Impl>()) {
    WipeOnExit wipe{password};
    if (iterations < kMinIterations || iterations > kMaxIterations) {
        throw std::invalid_argument("iterations outside what a reader accepts");
    }

    Impl& s = *impl_;
    std::memcpy(s.header.data(), kMagic, sizeof kMagic);
    PutLe(s.header.data() + 8, kVersion, 2);
    PutLe(s.header.data() + 10, iterations, 4);
    // Fresh per sink: a new salt is a new key, which keeps the record-index nonces unique
    if (BCryptGenRandom(nullptr, s.header.data() + kSaltAt, static_cast<ULONG>(kSaltBytes),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        Fail(ArchiveCode::kWriteFailed);
    }
    s.cipher.emplace(Derive(password, s.header, true));

    s.target = std::move(target);
    s.partial = PartialOf(s.target);
    // Beside the target, so the final move is a rename on the same volume; it only ever holds
    // ciphertext, and a stale one from a killed backup is replaced
    s.file = CreateFileW(s.partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) Fail(ArchiveCode::kWriteFailed);
    s.finished = false;
}

ArchiveFileSink::~ArchiveFileSink() = default;

void ArchiveFileSink::Begin(const Manifest& manifest) {
    impl_->Guarded([&] {
        if (impl_->next != 0) throw std::logic_error("begun twice");
        impl_->Write(impl_->header.data(), impl_->header.size());
        impl_->expected = manifest.consultations;
        impl_->WriteRecord(ToJson(manifest));
    });
}

void ArchiveFileSink::Add(const store::SessionRecord& record) {
    impl_->Guarded([&] {
        if (impl_->next == 0) throw std::logic_error("add before begin");
        if (impl_->ids.size() == impl_->expected) throw std::logic_error("more than the manifest");
        impl_->WriteRecord(ToJson(record));
        impl_->ids.push_back(record.id);
    });
}

void ArchiveFileSink::Commit() {
    Impl& s = *impl_;
    s.Guarded([&] {
        if (s.next == 0 || s.ids.size() != s.expected) throw std::logic_error("count mismatch");
        if (!FlushFileBuffers(s.file)) Fail(ArchiveCode::kWriteFailed);
        CloseHandle(s.file);
        s.file = INVALID_HANDLE_VALUE;
        s.Verify();
        if (!MoveFileExW(s.partial.c_str(), s.target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            Fail(ArchiveCode::kWriteFailed);
        }
        s.finished = true;
    });
}

}  // namespace clinicavt::archive
