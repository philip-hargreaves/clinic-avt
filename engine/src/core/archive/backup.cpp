#include "core/archive/backup.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

#include "core/archive/record_rules.hpp"
#include "core/common/iso8601.hpp"
#include "core/common/version.hpp"

namespace clinicavt::archive {

namespace {

bool Within(const Period& period, const std::string& at) {
    return (period.from.empty() || at >= period.from) && (period.to.empty() || at < period.to);
}

// What a backup may hold: finished real consultations, cleared ones included
bool Eligible(const store::SessionSummary& session) {
    return session.state == "finalised" && !session.demo;
}

bool HasAppraisal(const store::SessionRecord& record) {
    return std::ranges::any_of(record.documents, [](const store::RecordDocument& entry) {
        return entry.kind == store::DocumentKind::kReflection ||
               entry.kind == store::DocumentKind::kSummary;
    });
}

// Whether a record would give a cleared session something back
bool HasContent(const store::SessionRecord& record) {
    return !record.turns.empty() ||
           std::ranges::any_of(record.documents, [](const store::RecordDocument& entry) {
               return entry.kind == store::DocumentKind::kNote;
           });
}

// The record as a cleared session holds it: no transcript, no device, only the kept documents
store::SessionRecord Stripped(store::SessionRecord record) {
    record.device_id.clear();
    record.device_name.clear();
    record.lost_frames = 0;
    record.turns.clear();
    std::erase_if(record.documents, [](const store::RecordDocument& entry) {
        return !store::KeptOnClear(entry.kind);
    });
    return record;
}

bool OnlyKept(const store::SessionRecord& record) {
    return record.turns.empty() &&
           std::ranges::all_of(record.documents, [](const store::RecordDocument& entry) {
               return store::KeptOnClear(entry.kind);
           });
}

void Report(const Progress& progress, Phase phase, std::size_t done, std::size_t total) {
    if (progress) progress(phase, done, total);
}

}  // namespace

Counts Summarise(store::ISessionStore& store, const Period& period) {
    Counts counts;
    for (const store::SessionSummary& session : store.ListSessions()) {
        if (session.demo || !Within(period, session.started_at)) continue;
        if (!Eligible(session)) {
            counts.unfinished += 1;
            continue;
        }
        counts.consultations += 1;
        if (session.has_reflection) counts.reflections += 1;
    }
    return counts;
}

std::size_t Uncovered(store::ISessionStore& store, const Period& covered, const std::string& at) {
    std::size_t uncovered = 0;
    for (const store::SessionSummary& session : store.ListSessions()) {
        if (!Eligible(session)) continue;
        if (!Within(covered, session.started_at) || session.ended_at > at ||
            session.written_at > at) {
            uncovered += 1;
        }
    }
    return uncovered;
}

BackupResult BackUp(store::ISessionStore& store, const Period& period, IArchiveSink& sink,
                    const Progress& progress, bool reflections_only) {
    BackupResult result;
    result.manifest.created_at = Iso8601Now();
    result.manifest.reflections_only = reflections_only;
    std::vector<store::SessionId> selected;
    for (const store::SessionSummary& session : store.ListSessions()) {
        if (Eligible(session) && Within(period, session.started_at) &&
            (!reflections_only || session.has_reflection)) {
            selected.push_back(session.id);
        }
    }
    std::ranges::reverse(selected);  // oldest first

    result.manifest.from = period.from;
    result.manifest.to = period.to;
    result.manifest.consultations = selected.size();
    result.manifest.app_version = kVersion;
    sink.Begin(result.manifest);
    std::size_t written = 0;
    for (const store::SessionId& id : selected) {
        store::SessionRecord record = store.ReadRecord(id);
        if (reflections_only) record = Stripped(std::move(record));
        sink.Add(record);
        if (HasAppraisal(record)) result.reflections += 1;
        // A reflections-only file backs up no consultation
        if (!reflections_only) result.ids.push_back(id);
        Report(progress, Phase::kWriting, ++written, selected.size());
    }
    Report(progress, Phase::kChecking, selected.size(), selected.size());
    sink.Commit();
    return result;
}

RestoreResult Restore(store::ISessionStore& store, IArchiveSource& source, bool dry_run,
                      const Progress& progress) {
    RestoreResult expected;
    expected.manifest = source.GetManifest();
    const std::size_t total = expected.manifest.consultations;

    std::map<store::SessionId, bool> stored;  // id to cleared
    for (const store::SessionSummary& session : store.ListSessions()) {
        stored.emplace(session.id, session.cleared);
    }
    std::set<store::SessionId> seen;
    std::size_t read = 0;
    while (const std::optional<store::SessionRecord> record = source.Next()) {
        if (!ValidRecord(*record) || (expected.manifest.reflections_only && !OnlyKept(*record))) {
            throw ArchiveError(ArchiveCode::kDamaged);
        }
        const auto here = stored.find(record->id);
        const bool fresh = here == stored.end() && seen.insert(record->id).second;
        const bool completes = here != stored.end() && here->second && HasContent(*record);
        if (fresh) {
            expected.added += 1;
        } else if (completes) {
            expected.completed += 1;
            here->second = false;  // a second copy in the file finds it complete
        } else {
            expected.skipped += 1;
        }
        if ((fresh || completes) && HasAppraisal(*record)) expected.reflections += 1;
        Report(progress, Phase::kChecking, ++read, total);
    }
    if (dry_run) return expected;

    // The store decides each record again, inside its own transaction
    RestoreResult result;
    result.manifest = expected.manifest;
    source.Rewind();
    std::size_t done = 0;
    while (const std::optional<store::SessionRecord> record = source.Next()) {
        const store::AddOutcome outcome = store.AddRecord(*record);
        if (outcome == store::AddOutcome::kAdded) result.added += 1;
        if (outcome == store::AddOutcome::kCompleted) result.completed += 1;
        if (outcome == store::AddOutcome::kSkipped) {
            result.skipped += 1;
        } else if (HasAppraisal(*record)) {
            result.reflections += 1;
        }
        Report(progress, Phase::kRestoring, ++done, total);
    }
    return result;
}

}  // namespace clinicavt::archive
