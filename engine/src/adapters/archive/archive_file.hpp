#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "ports/session_archive.hpp"

namespace clinicavt::store {
class ChunkCipher;
}

namespace clinicavt::archive {

// PBKDF2 rounds for every backup written; the count travels in the header so a later writer can
// raise it
inline constexpr std::uint32_t kBackupIterations = 600000;

// A .clinicavt v1 file: a 30-byte plain header (magic, version, iterations, salt), then
// length-prefixed AES-GCM records under a key derived from the password and the header's salt.
// Record 0 is the manifest, then one record per consultation, then the end of the file.
// Every record authenticates the header and its own index, so nothing can be reordered,
// dropped, appended or spliced from another backup
class ArchiveFileSink final : public IArchiveSink {
   public:
    // Refuses a password under 12 code points before anything is written. The iteration count
    // is only lowered by tests
    ArchiveFileSink(std::filesystem::path target, std::string password,
                    std::uint32_t iterations = kBackupIterations);
    ~ArchiveFileSink() override;

    ArchiveFileSink(const ArchiveFileSink&) = delete;
    ArchiveFileSink& operator=(const ArchiveFileSink&) = delete;

    void Begin(const Manifest& manifest) override;
    void Add(const store::SessionRecord& record) override;

    // Reads the whole file back, then moves it over the target. Any failure, here or earlier,
    // deletes the partial file and leaves an existing target as it was
    void Commit() override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ArchiveFileSource final : public IArchiveSource {
   public:
    // Checks the header, derives the key and authenticates the manifest. Throws ArchiveError
    ArchiveFileSource(std::filesystem::path path, std::string password);
    ~ArchiveFileSource() override;

    ArchiveFileSource(const ArchiveFileSource&) = delete;
    ArchiveFileSource& operator=(const ArchiveFileSource&) = delete;

    const Manifest& GetManifest() const override;
    std::optional<store::SessionRecord> Next() override;
    void Rewind() override;

   private:
    friend class ArchiveFileSink;

    // The sink's check: the key it wrote with, so the password is not kept for it
    ArchiveFileSource(const std::filesystem::path& path, store::ChunkCipher&& cipher);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clinicavt::archive
