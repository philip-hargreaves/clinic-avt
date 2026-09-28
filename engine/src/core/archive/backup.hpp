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

// What a backup of a period would hold. Unfinished consultations (crashed, awaiting recovery)
// are counted and never included; demos are neither
struct Counts {
    std::size_t consultations = 0;
    std::size_t reflections = 0;  // of those, the ones with an appraisal entry
    std::size_t unfinished = 0;
};

Counts Summarise(store::ISessionStore& store, const Period& period);

// Consultations missing from a backup of `covered` made at `at`: outside its period, or ended
// or written to since
std::size_t Uncovered(store::ISessionStore& store, const Period& covered, const std::string& at);

enum class Phase { kWriting, kChecking, kRestoring };
using Progress = std::function<void(Phase phase, std::size_t done, std::size_t total)>;

struct BackupResult {
    std::vector<store::SessionId> ids;  // written and checked; none for reflections only
    std::size_t reflections = 0;
    Manifest manifest;
};

// Writes every finalised real consultation in the period, cleared ones included since their
// appraisal entry is a record, then commits the sink, which checks the whole file. Reflections
// only writes just the consultations with an appraisal entry, each as a cleared session would
// keep it, so the file holds nothing from the consultation itself
BackupResult BackUp(store::ISessionStore& store, const Period& period, IArchiveSink& sink,
                    const Progress& progress, bool reflections_only = false);

struct RestoreResult {
    std::size_t added = 0;
    std::size_t completed = 0;  // cleared here, given back their content
    std::size_t skipped = 0;    // already here
    std::size_t reflections = 0;
    Manifest manifest;

    std::size_t Restored() const {
        return added + completed;
    }
};

// Pass one reads and checks every record before anything is written, so a wrong password, a
// damaged file or a record the store would refuse leaves the store untouched. A dry run stops
// there with the counts it would reach. Pass two adds each record in its own transaction, so a
// stop part way leaves whole consultations and a rerun completes it
RestoreResult Restore(store::ISessionStore& store, IArchiveSource& source, bool dry_run,
                      const Progress& progress);

}  // namespace clinicavt::archive
