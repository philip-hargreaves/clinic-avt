#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>

#include "ports/session_store.hpp"

namespace clinicavt::archive {

// What a backup says about itself, sealed as its first record
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

// Fixed reasons, so no text from inside a backup can reach a log or the screen
enum class ArchiveCode {
    kWrongPassword,  // or a file that is not a backup of this format at all past its header
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

    // The wire's code for each reason
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

// Writes one backup: the manifest, then each session. Commit checks the whole file and only
// then publishes it; destroyed without Commit, nothing is left behind
class IArchiveSink {
   public:
    virtual ~IArchiveSink() = default;
    virtual void Begin(const Manifest& manifest) = 0;
    virtual void Add(const store::SessionRecord& record) = 0;
    virtual void Commit() = 0;
};

// Reads one backup. Opening authenticates the manifest; Next yields sessions in order and
// throws ArchiveError on any damage, including fewer or more records than the manifest says
class IArchiveSource {
   public:
    virtual ~IArchiveSource() = default;
    virtual const Manifest& GetManifest() const = 0;
    virtual std::optional<store::SessionRecord> Next() = 0;
    virtual void Rewind() = 0;
};

}  // namespace clinicavt::archive
