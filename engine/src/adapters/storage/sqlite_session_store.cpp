#include "adapters/storage/sqlite_session_store.hpp"

#include <cstdio>
#include <format>
#include <nlohmann/json.hpp>
#include <random>
#include <stdexcept>
#include <utility>

#include "adapters/storage/store_migrations.hpp"
#include "core/archive/record_rules.hpp"
#include "core/common/iso8601.hpp"

namespace clinicavt::store {

namespace {

// Max audio buffered in memory while commits fail; older frames are dropped and counted
constexpr std::chrono::seconds kPendingBound(30);
constexpr std::int64_t kSqlitePageLimit = 1073741823;  // the default max_page_count

struct KindSpec {
    const char* name;  // documents.kind
    Domain domain;     // sealing domain
};

KindSpec SpecFor(DocumentKind kind) {
    switch (kind) {
        case DocumentKind::kNote:
            return {"note", Domain::kNote};
        case DocumentKind::kPatient:
            return {"patient", Domain::kPatient};
        case DocumentKind::kTranslation:
            return {"translation", Domain::kTranslation};
        case DocumentKind::kLabel:
            return {"label", Domain::kLabel};
        case DocumentKind::kSummary:
            return {"summary", Domain::kSummary};
        case DocumentKind::kReflection:
            return {"reflection", Domain::kReflection};
        case DocumentKind::kGuidance:
            return {"guidance", Domain::kGuidance};
    }
    throw std::invalid_argument("unknown document kind");
}

std::string RandomId() {
    std::random_device device;
    std::string id;
    for (int i = 0; i < 4; ++i) id += std::format("{:08x}", device());
    return id;
}

// New slots start at a random sequence below 2^62 so a deleted and rewritten
// slot never reuses an IV
std::int64_t FreshSlotSequence() {
    std::random_device device;
    const std::uint64_t high = device();
    const std::uint64_t low = device();
    return static_cast<std::int64_t>(((high << 32 | low) >> 2) | 1);
}

std::span<const std::uint8_t> AsBytes(std::span<const float> frames) {
    return {reinterpret_cast<const std::uint8_t*>(frames.data()), frames.size_bytes()};
}

std::span<const std::uint8_t> AsBytes(const std::string& text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

}  // namespace

SqliteSessionStore::SqliteSessionStore(const std::filesystem::path& root,
                                       std::chrono::milliseconds commit_interval)
    : commit_interval_(commit_interval), db_(OpenDatabase(root)) {
    writer_ = std::thread([this] { WriterLoop(); });
}

SqliteSessionStore::~SqliteSessionStore() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    writer_.join();
    // Destruction does not finalise; an open session stays recoverable
    if (open_.has_value()) {
        try {
            CommitPending();
        } catch (const StoreError& e) {
            std::fprintf(stderr, "clinicavt-engine: store commit failed at close: %s\n", e.what());
        }
    }
}

SessionId SqliteSessionStore::Begin(const SessionMeta& meta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_.has_value()) throw StoreError(StoreCode::kBusy, "a session is already recording");

    Open session;
    session.id = RandomId();
    session.sample_rate = static_cast<std::uint64_t>(meta.sample_rate);
    session.cipher.emplace(ChunkCipher::Generate());
    const std::vector<std::uint8_t> wrapped = session.cipher->Wrapped();

    // Session row and key in one transaction
    Db::Transaction txn(db_);
    Db::Stmt insert = db_.Prepare(
        "INSERT INTO sessions(id, started_at, state, sample_rate, device_id, device_name, retain)"
        " VALUES(?, ?, 'recording', ?, ?, ?, ?)");
    insert.BindText(1, session.id);
    insert.BindText(2, meta.started_at.empty() ? Iso8601Now() : meta.started_at);
    insert.BindInt64(3, meta.sample_rate);
    insert.BindTextOrNull(4, meta.device_id);
    insert.BindTextOrNull(5, meta.device_name);
    insert.BindInt64(6, meta.retain ? 1 : 0);
    insert.Step();
    InsertKey(session.id, wrapped);
    txn.Commit();

    open_.emplace(std::move(session));
    {
        std::lock_guard<std::mutex> pending_lock(pending_mutex_);
        pending_ = {open_->id, {}, 0};
    }
    return open_->id;
}

void SqliteSessionStore::Append(const SessionId& id, std::span<const float> frames,
                                std::uint64_t lost_frames) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (pending_.id != id) {
        throw StoreError(StoreCode::kNotFound, "no open session with id " + id);
    }
    pending_.lost += lost_frames;
    pending_.frames.insert(pending_.frames.end(), frames.begin(), frames.end());
}

void SqliteSessionStore::TakePending(Open& session) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    session.held.insert(session.held.end(), pending_.frames.begin(), pending_.frames.end());
    session.held_lost += pending_.lost;
    pending_.frames.clear();
    pending_.lost = 0;
}

void SqliteSessionStore::ClosePending() {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_ = {};
}

// Timing in plaintext for queries; speaker and text encrypted
void SqliteSessionStore::InsertTurn(const SessionId& id, std::int64_t seq,
                                    const ChunkCipher& cipher, const asr::Turn& turn) {
    const std::string content =
        nlohmann::json{{"speaker", turn.speaker}, {"text", turn.text}}.dump();
    const std::vector<std::uint8_t> sealed =
        cipher.Seal(Domain::kTurns, id, static_cast<std::uint64_t>(seq), AsBytes(content));
    Db::Stmt insert = db_.Prepare(
        "INSERT INTO turns(session_id, seq, first_frame, frame_count, payload)"
        " VALUES(?, ?, ?, ?, ?)");
    insert.BindText(1, id);
    insert.BindInt64(2, seq);
    insert.BindInt64(3, static_cast<std::int64_t>(turn.first_frame));
    insert.BindInt64(4, static_cast<std::int64_t>(turn.frame_count));
    insert.BindBlob(5, sealed);
    insert.Step();
}

void SqliteSessionStore::ReplaceTurns(const SessionId& id, std::span<const asr::Turn> turns) {
    std::lock_guard<std::mutex> lock(mutex_);
    Open& session = RequireOpen(id);

    Db::Transaction txn(db_);
    Db::Stmt erase = db_.Prepare("DELETE FROM turns WHERE session_id = ?");
    erase.BindText(1, session.id);
    erase.Step();
    for (const asr::Turn& turn : turns) {
        // Sequence numbers continue from the last, so each sealed payload's AAD
        // is unique for the session
        InsertTurn(session.id, session.next_turn_seq, *session.cipher, turn);
        session.next_turn_seq += 1;
    }
    txn.Commit();
}

// Sealing erases the audio; it was kept only for crash recovery
void SqliteSessionStore::Finalise(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Open& session = RequireOpen(id);
    TakePending(session);
    const std::uint64_t lost = session.lost_committed + session.held_lost;

    Db::Transaction txn(db_);
    Db::Stmt erase = db_.Prepare("DELETE FROM chunks WHERE session_id = ?");
    erase.BindText(1, session.id);
    erase.Step();
    Db::Stmt update = db_.Prepare(
        "UPDATE sessions SET ended_at = ?, state = 'finalised', lost_frames = ? WHERE id = ?");
    update.BindText(1, Iso8601Now());
    update.BindInt64(2, static_cast<std::int64_t>(lost));
    update.BindText(3, session.id);
    update.Step();
    txn.Commit();
    db_.Exec("PRAGMA incremental_vacuum");

    open_.reset();
    ClosePending();
}

void SqliteSessionStore::Abandon(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    RequireOpen(id);
    CommitPending();
    // State stays 'recording', which marks it recoverable
    open_.reset();
    ClosePending();
}

void SqliteSessionStore::Cancel(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    RequireOpen(id);
    Erase(id);
    open_.reset();
    ClosePending();
}

void SqliteSessionStore::Delete(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_.has_value() && open_->id == id) {
        throw StoreError(StoreCode::kBusy, id + " is still recording");
    }
    Erase(id);
}

// Key row cascades with the session; leftover ciphertext in free pages is unreadable without it
void SqliteSessionStore::Erase(const SessionId& id) {
    Db::Stmt erase = db_.Prepare("DELETE FROM sessions WHERE id = ?");
    erase.BindText(1, id);
    if (EraseWhere(erase) == 0) throw StoreError(StoreCode::kNotFound, "no session " + id);
}

std::size_t SqliteSessionStore::EraseWhere(Db::Stmt& erase) {
    erase.Step();
    const auto removed = static_cast<std::size_t>(db_.QueryInt64("SELECT changes()"));
    if (removed > 0) Checkpoint();
    return removed;
}

void SqliteSessionStore::EraseUnretained() {
    std::lock_guard<std::mutex> lock(mutex_);
    Db::Stmt erase = db_.Prepare("DELETE FROM sessions WHERE retain = 0 AND state = 'finalised'");
    EraseWhere(erase);
}

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
        // Audio length from the plaintext turns (audio is gone after sealing); unsealed
        // sessions fall back to chunks
        " COALESCE((SELECT max(first_frame + frame_count) FROM turns t WHERE t.session_id = s.id),"
        "  (SELECT max(first_frame + frame_count) FROM chunks c WHERE c.session_id = s.id)),"
        " EXISTS(SELECT 1 FROM documents r WHERE r.session_id = s.id"
        "  AND r.kind IN ('reflection', 'summary')),"
        " s.demo, l.seq,"
        // Cleared: finalised, no turns, no note
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
        SessionSummary summary{select.ColumnText(0), select.ColumnText(1), select.ColumnText(2),
                               select.ColumnText(3), static_cast<int>(select.ColumnInt64(4))};
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

// Row, key and turns in one transaction, already finalised
SessionId SqliteSessionStore::Seed(const SessionSeed& seed) {
    std::lock_guard<std::mutex> lock(mutex_);
    const SessionId id = RandomId();
    const ChunkCipher cipher = ChunkCipher::Generate();

    Db::Transaction txn(db_);
    Db::Stmt insert = db_.Prepare(
        "INSERT INTO sessions(id, started_at, ended_at, state, sample_rate, retain, demo)"
        " VALUES(?, ?, ?, 'finalised', ?, 1, 1)");
    insert.BindText(1, id);
    insert.BindText(2, seed.started_at);
    insert.BindText(3, seed.ended_at);
    insert.BindInt64(4, seed.sample_rate);
    insert.Step();
    InsertKey(id, cipher.Wrapped());
    std::int64_t seq = 0;
    for (const asr::Turn& turn : seed.turns) {
        InsertTurn(id, seq, cipher, turn);
        seq += 1;
    }
    txn.Commit();
    return id;
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

std::size_t SqliteSessionStore::ClearDemo() {
    std::lock_guard<std::mutex> lock(mutex_);
    Db::Stmt erase = db_.Prepare("DELETE FROM sessions WHERE demo = 1");
    return EraseWhere(erase);
}

void SqliteSessionStore::RequireStored(const SessionId& id) {
    if (open_.has_value() && open_->id == id) {
        throw StoreError(StoreCode::kBusy, id + " is still recording");
    }
    Db::Stmt select = db_.Prepare("SELECT 1 FROM session_keys WHERE session_id = ?");
    select.BindText(1, id);
    if (!select.Step()) throw StoreError(StoreCode::kNotFound, "no session " + id);
}

ChunkCipher SqliteSessionStore::CipherFor(const SessionId& id) {
    RequireStored(id);
    Db::Stmt select = db_.Prepare("SELECT wrapped FROM session_keys WHERE session_id = ?");
    select.BindText(1, id);
    select.Step();
    return ChunkCipher::FromWrapped(select.ColumnBlob(0));
}

void SqliteSessionStore::InsertKey(const SessionId& id, std::span<const std::uint8_t> wrapped) {
    Db::Stmt key = db_.Prepare("INSERT INTO session_keys(session_id, wrapped) VALUES(?, ?)");
    key.BindText(1, id);
    key.BindBlob(2, wrapped);
    key.Step();
}

// Text is encrypted, metadata is not. The options row cascades from the note
// row, so a kind is written all or nothing
void SqliteSessionStore::WriteDocument(const SessionId& id, DocumentKind kind,
                                       const Document& document) {
    const ChunkCipher cipher = CipherFor(id);

    Db::Transaction txn(db_);
    // Slot's next sequence so a rewrite never reuses an IV
    std::int64_t seq = FreshSlotSequence();
    {
        Db::Stmt previous =
            db_.Prepare("SELECT seq FROM documents WHERE session_id = ? AND kind = ?");
        previous.BindText(1, id);
        previous.BindText(2, SpecFor(kind).name);
        if (previous.Step()) seq = previous.ColumnInt64(0) + 1;
    }
    WriteDocumentRow(id, kind, cipher, seq, document);
    txn.Commit();
}

void SqliteSessionStore::WriteDocumentRow(const SessionId& id, DocumentKind kind,
                                          const ChunkCipher& cipher, std::int64_t seq,
                                          const Document& document) {
    const KindSpec spec = SpecFor(kind);
    const std::vector<std::uint8_t> sealed =
        cipher.Seal(spec.domain, id, static_cast<std::uint64_t>(seq), AsBytes(document.text));
    Db::Stmt replace = db_.Prepare(
        "INSERT OR REPLACE INTO documents"
        "(session_id, kind, seq, language, payload, generated_at, edited_at)"
        " VALUES(?, ?, ?, ?, ?, ?, ?)");
    replace.BindText(1, id);
    replace.BindText(2, spec.name);
    replace.BindInt64(3, seq);
    replace.BindText(4, document.language);
    replace.BindBlob(5, sealed);
    replace.BindTextOrNull(6, document.generated_at);
    replace.BindTextOrNull(7, document.edited_at);
    replace.Step();
    if (kind == DocumentKind::kNote) {
        Db::Stmt options = db_.Prepare(
            "INSERT OR REPLACE INTO note_options(session_id, style, detail) VALUES(?, ?, ?)");
        options.BindText(1, id);
        options.BindText(2, document.style);
        options.BindText(3, document.detail);
        options.Step();
    }
}

void SqliteSessionStore::SaveDocument(const SessionId& id, DocumentKind kind,
                                      const Document& document) {
    std::lock_guard<std::mutex> lock(mutex_);
    Document generated = document;
    generated.generated_at = Iso8601Now();
    generated.edited_at.clear();
    WriteDocument(id, kind, generated);
}

void SqliteSessionStore::EditDocument(const SessionId& id, DocumentKind kind,
                                      const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    Document edited = ReadDocumentLocked(id, kind);
    edited.text = text;
    edited.edited_at = Iso8601Now();
    WriteDocument(id, kind, edited);
}

Document SqliteSessionStore::ReadDocument(const SessionId& id, DocumentKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ReadDocumentLocked(id, kind);
}

void SqliteSessionStore::DeleteDocument(const SessionId& id, DocumentKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    RequireStored(id);
    Db::Stmt remove = db_.Prepare("DELETE FROM documents WHERE session_id = ? AND kind = ?");
    remove.BindText(1, id);
    remove.BindText(2, SpecFor(kind).name);
    remove.Step();
}

Document SqliteSessionStore::ReadDocumentLocked(const SessionId& id, DocumentKind kind) {
    const ChunkCipher cipher = CipherFor(id);
    return ReadDocumentRow(id, kind, cipher).value_or(Document{});
}

std::optional<Document> SqliteSessionStore::ReadDocumentRow(const SessionId& id, DocumentKind kind,
                                                            const ChunkCipher& cipher) {
    const KindSpec spec = SpecFor(kind);
    Db::Stmt select = db_.Prepare(
        "SELECT d.language, d.payload, d.generated_at, d.edited_at, o.style, o.detail, d.seq"
        " FROM documents d LEFT JOIN note_options o ON o.session_id = d.session_id"
        " WHERE d.session_id = ? AND d.kind = ?");
    select.BindText(1, id);
    select.BindText(2, spec.name);
    if (!select.Step()) return std::nullopt;
    Document document;
    const auto plain = cipher.Open(
        spec.domain, id, static_cast<std::uint64_t>(select.ColumnInt64(6)), select.ColumnBlob(1));
    document.text.assign(plain.begin(), plain.end());
    document.language = select.ColumnText(0);
    document.generated_at = select.ColumnText(2);
    document.edited_at = select.ColumnText(3);
    document.revision = select.ColumnInt64(6);
    if (kind == DocumentKind::kNote) {
        document.style = select.ColumnText(4);
        document.detail = select.ColumnText(5);
    }
    return document;
}

std::vector<asr::Turn> SqliteSessionStore::ReadTurns(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ChunkCipher cipher = CipherFor(id);
    return ReadTurnsLocked(id, cipher);
}

std::vector<asr::Turn> SqliteSessionStore::ReadTurnsLocked(const SessionId& id,
                                                           const ChunkCipher& cipher) {
    Db::Stmt select = db_.Prepare(
        "SELECT seq, first_frame, frame_count, payload FROM turns WHERE session_id = ?"
        " ORDER BY seq");
    select.BindText(1, id);
    std::vector<asr::Turn> turns;
    while (select.Step()) {
        const auto plain =
            cipher.Open(Domain::kTurns, id, static_cast<std::uint64_t>(select.ColumnInt64(0)),
                        select.ColumnBlob(3));
        const auto content = nlohmann::json::parse(plain.begin(), plain.end());
        asr::Turn turn;
        turn.first_frame = static_cast<std::uint64_t>(select.ColumnInt64(1));
        turn.frame_count = static_cast<std::uint64_t>(select.ColumnInt64(2));
        turn.speaker = content.at("speaker").get<std::string>();
        turn.text = content.at("text").get<std::string>();
        turns.push_back(std::move(turn));
    }
    return turns;
}

std::vector<float> SqliteSessionStore::ReadAudio(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ChunkCipher cipher = CipherFor(id);
    Db::Stmt select =
        db_.Prepare("SELECT seq, payload FROM chunks WHERE session_id = ? ORDER BY seq");
    select.BindText(1, id);
    std::vector<float> audio;
    while (select.Step()) {
        const auto plain =
            cipher.Open(Domain::kAudio, id, static_cast<std::uint64_t>(select.ColumnInt64(0)),
                        select.ColumnBlob(1));
        const auto* frames = reinterpret_cast<const float*>(plain.data());
        const std::size_t count = plain.size() / sizeof(float);
        audio.insert(audio.end(), frames, frames + count);
    }
    return audio;
}

SqliteSessionStore::Open& SqliteSessionStore::RequireOpen(const SessionId& id) {
    if (!open_.has_value() || open_->id != id) {
        throw StoreError(StoreCode::kNotFound, "no open session with id " + id);
    }
    return *open_;
}

bool SqliteSessionStore::CommitPending() {
    Open& session = *open_;
    TakePending(session);
    if (session.held.empty() && session.held_lost == 0) return false;
    const std::vector<std::uint8_t> sealed =
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) set when the session opened
        session.cipher->Seal(Domain::kAudio, session.id,
                             static_cast<std::uint64_t>(session.next_seq), AsBytes(session.held));

    Db::Transaction txn(db_);
    Db::Stmt insert = db_.Prepare(
        "INSERT INTO chunks(session_id, seq, first_frame, frame_count, lost_before, payload)"
        " VALUES(?, ?, ?, ?, ?, ?)");
    insert.BindText(1, session.id);
    insert.BindInt64(2, session.next_seq);
    insert.BindInt64(3, static_cast<std::int64_t>(session.frames_committed));
    insert.BindInt64(4, static_cast<std::int64_t>(session.held.size()));
    insert.BindInt64(5, static_cast<std::int64_t>(session.held_lost));
    insert.BindBlob(6, sealed);
    insert.Step();
    txn.Commit();

    session.next_seq += 1;
    session.frames_committed += session.held.size();
    session.lost_committed += session.held_lost;
    session.held.clear();
    session.held_lost = 0;
    return true;
}

// Failed commits keep their audio for the next tick. Past kPendingBound the
// oldest frames are dropped as lost and the timeline shifts. The fault is
// reported once per episode
void SqliteSessionStore::WriterLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        cv_.wait_for(lock, commit_interval_, [this] { return stopping_; });
        if (stopping_) break;
        if (!open_.has_value()) continue;
        Open& session = *open_;
        try {
            if (CommitPending() && session.faulted) {
                session.faulted = false;
                std::fprintf(stderr, "clinicavt-engine: store commits again\n");
            }
        } catch (const StoreError& e) {
            const std::uint64_t bound = kPendingBound.count() * session.sample_rate;
            if (session.held.size() > bound) {
                const auto dropped = session.held.size() - bound;
                session.held.erase(session.held.begin(),
                                   session.held.begin() + static_cast<std::ptrdiff_t>(dropped));
                session.frames_committed += dropped;
                session.held_lost += dropped;
            }
            if (!session.faulted) {
                session.faulted = true;
                std::fprintf(stderr, "clinicavt-engine: store commit failed: %s\n", e.what());
                if (on_fault_) {
                    const auto listener = on_fault_;
                    lock.unlock();
                    listener(e);
                    lock.lock();
                }
            }
        }
    }
}

void SqliteSessionStore::SetFaultListener(std::function<void(const StoreError&)> listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_fault_ = std::move(listener);
}

void SqliteSessionStore::SetMaxPageCount(std::int64_t pages) {
    std::lock_guard<std::mutex> lock(mutex_);
    db_.Exec(
        ("PRAGMA max_page_count=" + std::to_string(pages > 0 ? pages : kSqlitePageLimit)).c_str());
}

void SqliteSessionStore::Checkpoint() {
    if (!db_.CheckpointTruncate()) {
        std::fprintf(stderr, "clinicavt-engine: store log kept, a reader holds it\n");
    }
}

}  // namespace clinicavt::store
