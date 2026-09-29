#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>

#include "ports/session_store.hpp"

namespace clinicavt::archive {

// Backup metadata, sealed as the first record
struct Manifest {
    int version = 1;
    std::string created_at;  // ISO 8601 UTC
    std::string from;        // the period asked for, half-open, empty when open
    std::string to;
    std::size_t consultations = 0;
    bool transcripts = true;
    bool reflections_only = false;  // each record holds only what a cleared session keeps
    std::string app_version;
};

// Fixed codes so no backup content reaches logs or the UI
enum class ArchiveCode {
    kWrongPassword,  // also a non-backup file whose header happens to parse
    kNotABackup,     // the header is not ours
    kNewerVersion,
    kDamaged,  // authentication, order, count or bounds failed
    kWeakPassword,
    kWriteFailed,
    kReadFailed,
};

class ArchiveError : public std::runtime_error {
   public:
    explicit ArchiveError(ArchiveCode code) : std::runtime_error(Name(code)), code_(code) {}
    ArchiveCode Code() const {
        return code_;
    }

    // Wire name for each code
    static const char* Name(ArchiveCode code) {
        switch (code) {
            case ArchiveCode::kWrongPassword:
                return "wrong-password";
            case ArchiveCode::kNotABackup:
                return "not-a-backup";
            case ArchiveCode::kNewerVersion:
                return "newer-version";
            case ArchiveCode::kDamaged:
                return "damaged";
            case ArchiveCode::kWeakPassword:
                return "weak-password";
            case ArchiveCode::kWriteFailed:
                return "write-failed";
            case ArchiveCode::kReadFailed:
                return "read-failed";
        }
        return "damaged";
    }

   private:
    ArchiveCode code_;
};

// Writes one backup: manifest, then each session. Commit verifies the whole file
// before publishing; destroying without Commit leaves nothing
class IArchiveSink {
   public:
    virtual ~IArchiveSink() = default;
    virtual void Begin(const Manifest& manifest) = 0;
    virtual void Add(const store::SessionRecord& record) = 0;
    virtual void Commit() = 0;
};

// Reads one backup. Opening authenticates the manifest; Next yields sessions in
// order and throws ArchiveError on damage, including a record count mismatch
class IArchiveSource {
   public:
    virtual ~IArchiveSource() = default;
    virtual const Manifest& GetManifest() const = 0;
    virtual std::optional<store::SessionRecord> Next() = 0;
    virtual void Rewind() = 0;
};

}  // namespace clinicavt::archive
