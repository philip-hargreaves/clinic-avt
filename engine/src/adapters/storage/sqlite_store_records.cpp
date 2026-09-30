#include <string>
#include <utility>
#include <vector>

#include "adapters/storage/sqlite_session_store.hpp"
#include "adapters/storage/sqlite_store_rows.hpp"
#include "core/archive/record_rules.hpp"

namespace clinicavt::store {

using rows::FreshSlotSequence;
using rows::SpecFor;

// Labels are encrypted, so each row is opened with its own key. edited_at is the
// latest across the session's documents
std::vector<SessionSummary> SqliteSessionStore::ListSessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionSummary> sessions;
    Db::Stmt select = db_.Prepare(
        "SELECT s.id, s.started_at, s.ended_at, s.state, s.sample_rate, k.wrapped, l.payload,"
        // Label, summary, reflection and guidance writes are not record edits
        " (SELECT max(edited_at) FROM documents d WHERE d.session_id = s.id"
        "  AND d.kind NOT IN ('label', 'summary', 'reflection', 'guidance')),"
        // Audio length comes from the plaintext turns, since audio is gone after sealing.
        // Unsealed sessions use the chunks
        " COALESCE((SELECT max(first_frame + frame_count) FROM turns t WHERE t.session_id = s.id),"
        "  (SELECT max(first_frame + frame_count) FROM chunks c WHERE c.session_id = s.id)),"
        " EXISTS(SELECT 1 FROM documents r WHERE r.session_id = s.id"
        "  AND r.kind IN ('reflection', 'summary')),"
        " s.demo, l.seq,"
        // Cleared means finalised with no turns and no note
        " s.state = 'finalised' AND NOT EXISTS(SELECT 1 FROM turns u WHERE u.session_id = s.id)"
        "  AND NOT EXISTS(SELECT 1 FROM documents n WHERE n.session_id = s.id AND n.kind = 'note'),"
        " (SELECT max(COALESCE(max(w.generated_at), ''), COALESCE(max(w.edited_at), ''))"
        "  FROM documents w WHERE w.session_id = s.id)"
        " FROM sessions s"
        " LEFT JOIN session_keys k ON k.session_id = s.id"
        " LEFT JOIN documents l ON l.session_id = s.id AND l.kind = 'label'"
        // Hide finalised retain-off sessions (erased on leave). Crashed ones stay for recovery
        " WHERE NOT (s.retain = 0 AND s.state = 'finalised')"
        " ORDER BY s.started_at DESC, s.rowid DESC");
    while (select.Step()) {
        // The schema's CHECK allows only recording and finalised
        SessionSummary summary{select.ColumnText(0), select.ColumnText(1), select.ColumnText(2),
                               select.ColumnText(3) == "finalised" ? SessionState::kFinalised
                                                                   : SessionState::kRecording,
                               static_cast<int>(select.ColumnInt64(4))};
        const std::vector<std::uint8_t> sealed = select.ColumnBlob(6);
        if (!sealed.empty()) {
            try {
                const ChunkCipher cipher = ChunkCipher::FromWrapped(select.ColumnBlob(5));
                const auto plain =
                    cipher.Open(Domain::kLabel, summary.id,
                                static_cast<std::uint64_t>(select.ColumnInt64(11)), sealed);
                summary.label.assign(plain.begin(), plain.end());
            } catch (const StoreError&) {  // NOLINT(bugprone-empty-catch) listed without a label
            }
        }
        summary.edited_at = select.ColumnText(7);
        if (summary.sample_rate > 0) {
            summary.audio_seconds =
                static_cast<double>(select.ColumnInt64(8)) / summary.sample_rate;
        }
        summary.has_reflection = select.ColumnInt64(9) != 0;
        summary.demo = select.ColumnInt64(10) != 0;
        summary.cleared = select.ColumnInt64(12) != 0;
        summary.written_at = select.ColumnText(13);
        sessions.push_back(std::move(summary));
    }
    return sessions;
}

std::size_t SqliteSessionStore::DeleteAll(bool keep_reflections) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string live = open_.has_value() ? open_->id : std::string();
    if (!keep_reflections) {
        Db::Stmt erase = db_.Prepare("DELETE FROM sessions WHERE id <> ?");
        erase.BindText(1, live);
        return EraseWhere(erase);
    }

    // Already-cleared sessions are not counted again
    std::vector<SessionId> to_clear;
    {
        Db::Stmt select = db_.Prepare(
            "SELECT s.id FROM sessions s WHERE s.id <> ? AND s.state = 'finalised'"
            " AND EXISTS(SELECT 1 FROM documents a WHERE a.session_id = s.id"
            "  AND a.kind IN ('reflection', 'summary'))"
            " AND (EXISTS(SELECT 1 FROM turns t WHERE t.session_id = s.id)"
            "  OR EXISTS(SELECT 1 FROM chunks c WHERE c.session_id = s.id)"
            "  OR EXISTS(SELECT 1 FROM documents d WHERE d.session_id = s.id"
            "   AND d.kind NOT IN ('reflection', 'summary', 'label')))");
        select.BindText(1, live);
        while (select.Step()) to_clear.push_back(select.ColumnText(0));
    }
    Db::Transaction txn(db_);
    for (const SessionId& id : to_clear) ClearLocked(id);
    Db::Stmt erase = db_.Prepare(
        "DELETE FROM sessions WHERE id <> ? AND NOT (state = 'finalised'"
        " AND EXISTS(SELECT 1 FROM documents a WHERE a.session_id = sessions.id"
        "  AND a.kind IN ('reflection', 'summary')))");
    erase.BindText(1, live);
    erase.Step();
    const auto affected =
        to_clear.size() + static_cast<std::size_t>(db_.QueryInt64("SELECT changes()"));
    txn.Commit();
    if (affected > 0) {
        db_.Exec("PRAGMA incremental_vacuum");
        Checkpoint();
    }
    return affected;
}

void SqliteSessionStore::Clear(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Db::Transaction txn(db_);
    ClearLocked(id);
    txn.Commit();
    db_.Exec("PRAGMA incremental_vacuum");
    Checkpoint();
}

// Reseal kept documents under a new key and delete the old key row, so erased
// rows left in free pages are unreadable, as after Delete
void SqliteSessionStore::ClearLocked(const SessionId& id) {
    const ChunkCipher previous = CipherFor(id);
    {
        Db::Stmt state = db_.Prepare("SELECT state FROM sessions WHERE id = ?");
        state.BindText(1, id);
        state.Step();
        if (state.ColumnText(0) != "finalised") {
            throw StoreError(StoreCode::kBusy, id + " was never finalised");
        }
    }
    std::vector<std::pair<DocumentKind, Document>> kept;
    bool appraisal = false;
    for (const DocumentKind kind : kKeptOnClear) {
        if (auto document = ReadDocumentRow(id, kind, previous)) {
            appraisal = appraisal || kind != DocumentKind::kLabel;
            kept.emplace_back(kind, std::move(*document));
        }
    }
    auto erase = [&](const char* sql) {
        Db::Stmt statement = db_.Prepare(sql);
        statement.BindText(1, id);
        statement.Step();
    };
    if (!appraisal) {
        erase("DELETE FROM sessions WHERE id = ?");
        return;
    }
    erase("DELETE FROM chunks WHERE session_id = ?");
    erase("DELETE FROM turns WHERE session_id = ?");
    erase("DELETE FROM documents WHERE session_id = ?");
    erase("DELETE FROM session_keys WHERE session_id = ?");
    const ChunkCipher fresh = ChunkCipher::Generate();
    InsertKey(id, fresh.Wrapped());
    // New key, so slots keep their sequences without IV reuse
    for (const auto& [kind, document] : kept) {
        WriteDocumentRow(id, kind, fresh, document.revision, document);
    }
}

SessionRecord SqliteSessionStore::ReadRecord(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ChunkCipher cipher = CipherFor(id);
    SessionRecord record;
    {
        Db::Stmt select = db_.Prepare(
            "SELECT state, started_at, ended_at, sample_rate, device_id, device_name, lost_frames"
            " FROM sessions WHERE id = ?");
        select.BindText(1, id);
        select.Step();
        if (select.ColumnText(0) != "finalised") {
            throw StoreError(StoreCode::kBusy, id + " was never finalised");
        }
        record.id = id;
        record.started_at = select.ColumnText(1);
        record.ended_at = select.ColumnText(2);
        record.sample_rate = static_cast<int>(select.ColumnInt64(3));
        record.device_id = select.ColumnText(4);
        record.device_name = select.ColumnText(5);
        record.lost_frames = static_cast<std::uint64_t>(select.ColumnInt64(6));
    }
    record.turns = ReadTurnsLocked(id, cipher);
    for (const DocumentKind kind : kDocumentKinds) {
        if (auto document = ReadDocumentRow(id, kind, cipher)) {
            record.documents.push_back({kind, std::move(*document)});
        }
    }
    return record;
}

// The insert is the existence check, inside the transaction
AddOutcome SqliteSessionStore::AddRecord(const SessionRecord& record) {
    if (!archive::ValidRecord(record)) {
        throw StoreError(StoreCode::kOther, "the record is not one this store could hold");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Db::Transaction txn(db_);
    Db::Stmt insert = db_.Prepare(
        "INSERT INTO sessions(id, started_at, ended_at, state, sample_rate, device_id,"
        " device_name, lost_frames, retain, demo) VALUES(?, ?, ?, 'finalised', ?, ?, ?, ?, 1, 0)"
        " ON CONFLICT(id) DO NOTHING");
    insert.BindText(1, record.id);
    insert.BindText(2, record.started_at);
    insert.BindText(3, record.ended_at);
    insert.BindInt64(4, record.sample_rate);
    insert.BindTextOrNull(5, record.device_id);
    insert.BindTextOrNull(6, record.device_name);
    insert.BindInt64(7, static_cast<std::int64_t>(record.lost_frames));
    insert.Step();
    if (db_.QueryInt64("SELECT changes()") == 0) {
        const AddOutcome outcome = CompleteLocked(record);
        if (outcome == AddOutcome::kCompleted) txn.Commit();
        return outcome;
    }

    const ChunkCipher cipher = ChunkCipher::Generate();
    InsertKey(record.id, cipher.Wrapped());
    std::int64_t seq = 0;
    for (const asr::Turn& turn : record.turns) {
        InsertTurn(record.id, seq, cipher, turn);
        seq += 1;
    }
    // New key, so stored revisions can be sequences without IV reuse
    for (const RecordDocument& entry : record.documents) {
        WriteDocumentRow(record.id, entry.kind, cipher, entry.document.revision, entry.document);
    }
    txn.Commit();
    return AddOutcome::kAdded;
}

// Restoring onto a cleared copy of the same session adds back the transcript
// and missing documents. Kept documents are unchanged
AddOutcome SqliteSessionStore::CompleteLocked(const SessionRecord& record) {
    {
        Db::Stmt cleared = db_.Prepare(
            "SELECT s.state = 'finalised'"
            " AND NOT EXISTS(SELECT 1 FROM turns t WHERE t.session_id = s.id)"
            " AND NOT EXISTS(SELECT 1 FROM documents n WHERE n.session_id = s.id"
            "  AND n.kind = 'note')"
            " FROM sessions s WHERE s.id = ?");
        cleared.BindText(1, record.id);
        if (!cleared.Step() || cleared.ColumnInt64(0) == 0) return AddOutcome::kSkipped;
    }
    const ChunkCipher cipher = CipherFor(record.id);
    bool added = false;
    // Clear sealed only the kept kinds under this key, so other kinds can use the
    // record revision. Turns have no revision and start at a random sequence
    std::int64_t seq = FreshSlotSequence();
    for (const asr::Turn& turn : record.turns) {
        InsertTurn(record.id, seq, cipher, turn);
        seq += 1;
        added = true;
    }
    for (const RecordDocument& entry : record.documents) {
        Db::Stmt held = db_.Prepare("SELECT 1 FROM documents WHERE session_id = ? AND kind = ?");
        held.BindText(1, record.id);
        held.BindText(2, SpecFor(entry.kind).name);
        if (held.Step()) continue;
        // A kept kind now missing was deleted after the clear and may have used the
        // record revision under this key, so start a fresh sequence
        WriteDocumentRow(record.id, entry.kind, cipher,
                         KeptOnClear(entry.kind) ? FreshSlotSequence() : entry.document.revision,
                         entry.document);
        added = true;
    }
    return added ? AddOutcome::kCompleted : AddOutcome::kSkipped;
}

}  // namespace clinicavt::store
