#include <array>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "adapters/storage/sqlite_session_store.hpp"
#include "adapters/storage/sqlite_store_rows.hpp"
#include "core/archive/record_rules.hpp"

namespace clinicavt::store {

using rows::ClearedSql;
using rows::FreshSlotSequence;
using rows::HasAppraisalSql;
using rows::KindList;
using rows::SpecFor;

namespace {

constexpr std::array kNotRecordEdits{DocumentKind::kLabel, DocumentKind::kSummary,
                                     DocumentKind::kReflection, DocumentKind::kGuidance};

const std::string& ListSql() {
    static const std::string kSql = std::format(
        "SELECT c.id, c.started_at, c.ended_at, c.state, c.sample_rate, k.wrapped_key,"
        " l.encrypted_text, l.revision,"
        " (SELECT max(edited_at) FROM documents e WHERE e.consultation_id = c.id"
        "  AND e.kind NOT IN ({})),"
        // Audio length comes from the turns once finalised and from the chunks before that
        " COALESCE((SELECT max(first_frame + frame_count) FROM turns t WHERE t.consultation_id = "
        "c.id),"
        "  (SELECT max(first_frame + frame_count) FROM audio_chunks a"
        "   WHERE a.consultation_id = c.id)),"
        " {}, c.sample, {},"
        " (SELECT max(COALESCE(max(w.generated_at), ''), COALESCE(max(w.edited_at), ''))"
        "  FROM documents w WHERE w.consultation_id = c.id)"
        " FROM consultations c"
        " LEFT JOIN consultation_keys k ON k.consultation_id = c.id"
        " LEFT JOIN documents l ON l.consultation_id = c.id AND l.kind = 'label'"
        // Finalised unsaved sessions are erased when left. Crashed ones stay for recovery
        " WHERE NOT (c.saved = 0 AND c.state = 'finalised')"
        " ORDER BY c.started_at DESC, c.rowid DESC",
        KindList(kNotRecordEdits), HasAppraisalSql(), ClearedSql());
    return kSql;
}

}  // namespace

// Labels are sealed, so each row is opened with its own key
std::vector<SessionSummary> SqliteSessionStore::ListSessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionSummary> sessions;
    Db::Stmt select = db_.Prepare(ListSql().c_str());
    while (select.Step()) {
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
                                static_cast<std::uint64_t>(select.ColumnInt64(7)), sealed);
                summary.label.assign(plain.begin(), plain.end());
            } catch (const StoreError&) {  // NOLINT(bugprone-empty-catch) listed without a label
            }
        }
        summary.edited_at = select.ColumnText(8);
        if (summary.sample_rate > 0) {
            summary.audio_seconds =
                static_cast<double>(select.ColumnInt64(9)) / summary.sample_rate;
        }
        summary.has_reflection = select.ColumnInt64(10) != 0;
        summary.sample = select.ColumnInt64(11) != 0;
        summary.cleared = select.ColumnInt64(12) != 0;
        summary.written_at = select.ColumnText(13);
        sessions.push_back(std::move(summary));
    }
    return sessions;
}

bool SqliteSessionStore::Cleared(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Db::Stmt select =
        db_.Prepare(("SELECT " + ClearedSql() + " FROM consultations c WHERE c.id = ?").c_str());
    select.BindText(1, id);
    if (!select.Step()) throw StoreError(StoreCode::kNotFound, "no session " + id);
    return select.ColumnInt64(0) != 0;
}

std::size_t SqliteSessionStore::DeleteAll(bool keep_reflections) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string live = open_.has_value() ? open_->id : std::string();
    if (!keep_reflections) {
        Db::Stmt erase = db_.Prepare("DELETE FROM consultations WHERE id <> ?");
        erase.BindText(1, live);
        return EraseWhere(erase);
    }

    // Already-cleared sessions are not counted again
    std::vector<SessionId> to_clear;
    {
        Db::Stmt select = db_.Prepare(
            ("SELECT c.id FROM consultations c WHERE c.id <> ? AND c.state = 'finalised' AND " +
             HasAppraisalSql() + " AND NOT " + ClearedSql())
                .c_str());
        select.BindText(1, live);
        while (select.Step()) to_clear.push_back(select.ColumnText(0));
    }
    Db::Transaction txn(db_);
    for (const SessionId& id : to_clear) ClearLocked(id);
    Db::Stmt erase = db_.Prepare(
        ("DELETE FROM consultations WHERE id <> ? AND id NOT IN (SELECT c.id FROM consultations c"
         " WHERE c.state = 'finalised' AND " +
         HasAppraisalSql() + ")")
            .c_str());
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

// The kept documents are resealed under a new key and the old key is deleted, so the erased rows
// left in free pages cannot be read
void SqliteSessionStore::ClearLocked(const SessionId& id) {
    const ChunkCipher previous = CipherFor(id);
    {
        Db::Stmt state = db_.Prepare("SELECT state FROM consultations WHERE id = ?");
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
            appraisal = appraisal || IsAppraisal(kind);
            kept.emplace_back(kind, std::move(*document));
        }
    }
    auto erase = [&](const char* sql) {
        Db::Stmt statement = db_.Prepare(sql);
        statement.BindText(1, id);
        statement.Step();
    };
    if (!appraisal) {
        erase("DELETE FROM consultations WHERE id = ?");
        return;
    }
    erase("DELETE FROM audio_chunks WHERE consultation_id = ?");
    erase("DELETE FROM turns WHERE consultation_id = ?");
    erase("DELETE FROM documents WHERE consultation_id = ?");
    erase("DELETE FROM consultation_keys WHERE consultation_id = ?");
    const ChunkCipher fresh = ChunkCipher::Generate();
    InsertKey(id, fresh.Wrapped());
    // The key is new, so the old revisions repeat no IV
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
            "SELECT state, started_at, ended_at, sample_rate, device_id, device_name,"
            " dropped_frames FROM consultations WHERE id = ?");
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
        "INSERT INTO consultations(id, started_at, ended_at, state, sample_rate, device_id,"
        " device_name, dropped_frames, saved, sample)"
        " VALUES(?, ?, ?, 'finalised', ?, ?, ?, ?, 1, 0) ON CONFLICT(id) DO NOTHING");
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
    // The key is new, so the backup's revisions repeat no IV
    for (const RecordDocument& entry : record.documents) {
        WriteDocumentRow(record.id, entry.kind, cipher, entry.document.revision, entry.document);
    }
    txn.Commit();
    return AddOutcome::kAdded;
}

// Restoring onto a cleared copy of the same session adds back the transcript and the missing
// documents. The kept documents stay as they are
AddOutcome SqliteSessionStore::CompleteLocked(const SessionRecord& record) {
    {
        Db::Stmt cleared = db_.Prepare(
            ("SELECT " + ClearedSql() + " FROM consultations c WHERE c.id = ?").c_str());
        cleared.BindText(1, record.id);
        if (!cleared.Step() || cleared.ColumnInt64(0) == 0) return AddOutcome::kSkipped;
    }
    const ChunkCipher cipher = CipherFor(record.id);
    bool added = false;
    // Clear resealed only the kept kinds under this key, so the other kinds can take the
    // backup's revision. Turns start at a random sequence
    std::int64_t seq = FreshSlotSequence();
    for (const asr::Turn& turn : record.turns) {
        InsertTurn(record.id, seq, cipher, turn);
        seq += 1;
        added = true;
    }
    for (const RecordDocument& entry : record.documents) {
        Db::Stmt held =
            db_.Prepare("SELECT 1 FROM documents WHERE consultation_id = ? AND kind = ?");
        held.BindText(1, record.id);
        held.BindText(2, SpecFor(entry.kind).name);
        if (held.Step()) continue;
        // A kept kind missing now was deleted after the clear and may have used the backup's
        // revision under this key
        WriteDocumentRow(record.id, entry.kind, cipher,
                         KeptOnClear(entry.kind) ? FreshSlotSequence() : entry.document.revision,
                         entry.document);
        added = true;
    }
    return added ? AddOutcome::kCompleted : AddOutcome::kSkipped;
}

}  // namespace clinicavt::store
