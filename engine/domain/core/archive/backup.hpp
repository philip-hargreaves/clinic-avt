#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "ports/session_archive.hpp"
#include "ports/session_store.hpp"

namespace clinicavt::archive {

// Half-open [from, to) over started_at, ISO 8601 UTC; an empty end is open
struct Period {
    std::string from;
    std::string to;
};

// Counts for a backup of a period. Unfinished sessions (crashed, awaiting recovery) are counted but
// never included. Demos are excluded
struct Counts {
    std::size_t consultations = 0;
    std::size_t reflections = 0;  // with an appraisal entry
    std::size_t unfinished = 0;
};

Counts Summarise(store::ISessionStore& store, const Period& period);

// Consultations missing from a backup of `covered` made at `at`: outside its period, or ended
// or written to since
std::size_t Uncovered(store::ISessionStore& store, const Period& covered, const std::string& at);

enum class Phase { kWriting, kChecking, kRestoring };
using Progress = std::function<void(Phase phase, std::size_t done, std::size_t total)>;

struct BackupResult {
    std::vector<store::SessionId> ids;  // written and checked, empty if reflections_only
    std::size_t reflections = 0;
    Manifest manifest;
};

// Writes finalised non-demo consultations in the period (cleared ones too, for their appraisal
// entry), then commits the sink, which checks the file. With reflections_only it writes only those
// with an appraisal entry, stripped as on clear
BackupResult BackUp(store::ISessionStore& store, const Period& period, IArchiveSink& sink,
                    const Progress& progress, bool reflections_only);

struct RestoreResult {
    std::size_t added = 0;
    std::size_t completed = 0;  // were cleared locally and had their content restored
    std::size_t skipped = 0;    // already here
    std::size_t reflections = 0;
    Manifest manifest;

    std::size_t Restored() const {
        return added + completed;
    }
};

// Validates every record before writing any, so a wrong password, damaged file or refused record
// changes nothing. dry_run returns at that point with the counts. Each record is then added in its
// own transaction, so an interrupted restore leaves whole consultations and a rerun completes it
RestoreResult Restore(store::ISessionStore& store, IArchiveSource& source, bool dry_run,
                      const Progress& progress);

}  // namespace clinicavt::archive
