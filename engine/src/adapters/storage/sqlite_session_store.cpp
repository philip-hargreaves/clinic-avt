#include "adapters/storage/sqlite_session_store.hpp"

#include <cstdio>
#include <format>
#include <nlohmann/json.hpp>
#include <random>
#include <stdexcept>
#include <utility>

#include "adapters/storage/store_migrations.hpp"
#include "core/common/iso8601.hpp"

namespace clinicavt::store {

namespace {

// Audio held in memory while the disk refuses commits. Older frames are dropped and counted
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
    // Destruction is not finalisation. An open session stays recoverable with what it buffered
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

    // Row and key land together: a session either exists with its key or
    // not at all
    Db::Transaction txn(db_);
    Db::Stmt insert = db_.Prepare(
        "INSERT INTO sessions(id, started_at, state, sample_rate, device_id, device_name, retain)"
        " VALUES(?, ?, 'recording', ?, ?, ?, ?)");
    insert.BindText(1, session.id);
    insert.BindText(2, Iso8601Now());
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

// Timing is queryable shape. Speaker and text are content, so encrypted
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
        // Sequence numbers continue rather than restart, so every sealed
        // payload's AAD stays unique for the session's lifetime
        InsertTurn(session.id, session.next_turn_seq, *session.cipher, turn);
        session.next_turn_seq += 1;
    }
    txn.Commit();
}

// The seal erases the audio. The transcript is the record, and the recording
// existed only to resume a crash
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
    // No state change: recording is what marks it recoverable
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

// The key row goes with the session (cascade): whatever ciphertext lingers
// in free pages afterwards is noise without it
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

// The label is content, so each row's is opened with its own key. The
// edit stamp is the latest over the session's documents
std::vector<SessionSummary> SqliteSessionStore::ListSessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionSummary> sessions;
    Db::Stmt select = db_.Prepare(
        "SELECT s.id, s.started_at, s.ended_at, s.state, s.sample_rate, k.wrapped, l.payload,"
        // Label, summary, reflection and guidance are not edits to the record
        " (SELECT max(edited_at) FROM documents d WHERE d.session_id = s.id"
        "  AND d.kind NOT IN ('label', 'summary', 'reflection', 'guidance')),"
        // The audio's length outlives the audio: the turns' end is plaintext.
        // A session that never sealed still has its chunks
        " COALESCE((SELECT max(first_frame + frame_count) FROM turns t WHERE t.session_id = s.id),"
        "  (SELECT max(first_frame + frame_count) FROM chunks c WHERE c.session_id = s.id)),"
        " EXISTS(SELECT 1 FROM documents r WHERE r.session_id = s.id"
        "  AND r.kind IN ('reflection', 'summary')),"
        " s.demo, l.seq"
        " FROM sessions s"
        " LEFT JOIN session_keys k ON k.session_id = s.id"
        " LEFT JOIN documents l ON l.session_id = s.id AND l.kind = 'label'"
        // A keep-off session exists only until it is left. History never
        // shows what is not being kept. Crashed ones stay for recovery
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
            } catch (const StoreError&) {  // one unreadable label never hides the list
            }
        }
        summary.edited_at = select.ColumnText(7);
        if (summary.sample_rate > 0) {
            summary.audio_seconds =
                static_cast<double>(select.ColumnInt64(8)) / summary.sample_rate;
        }
        summary.has_reflection = select.ColumnInt64(9) != 0;
        summary.demo = select.ColumnInt64(10) != 0;
        sessions.push_back(std::move(summary));
    }
    return sessions;
}

// Row, key and turns land in one transaction, already finalised
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

std::size_t SqliteSessionStore::DeleteAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    Db::Stmt erase = db_.Prepare("DELETE FROM sessions WHERE id <> ?");
    erase.BindText(1, open_.has_value() ? open_->id : std::string());
    return EraseWhere(erase);
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

// Text is content, so sealed. The rest is shape. The options row follows
// the note row (cascade), so a kind is all or nothing
void SqliteSessionStore::WriteDocument(const SessionId& id, DocumentKind kind,
                                       const Document& document) {
    const ChunkCipher cipher = CipherFor(id);
    const KindSpec spec = SpecFor(kind);

    Db::Transaction txn(db_);
    // The slot's next sequence: a rewrite never reseals under a used IV
    std::int64_t seq = 1;
    {
        Db::Stmt previous =
            db_.Prepare("SELECT seq FROM documents WHERE session_id = ? AND kind = ?");
        previous.BindText(1, id);
        previous.BindText(2, spec.name);
        if (previous.Step()) seq = previous.ColumnInt64(0) + 1;
    }
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
    txn.Commit();
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
    const KindSpec spec = SpecFor(kind);
    Document document;
    Db::Stmt select = db_.Prepare(
        "SELECT d.language, d.payload, d.generated_at, d.edited_at, o.style, o.detail, d.seq"
        " FROM documents d LEFT JOIN note_options o ON o.session_id = d.session_id"
        " WHERE d.session_id = ? AND d.kind = ?");
    select.BindText(1, id);
    select.BindText(2, spec.name);
    if (select.Step()) {
        const auto plain =
            cipher.Open(spec.domain, id, static_cast<std::uint64_t>(select.ColumnInt64(6)),
                        select.ColumnBlob(1));
        document.text.assign(plain.begin(), plain.end());
        document.language = select.ColumnText(0);
        document.generated_at = select.ColumnText(2);
        document.edited_at = select.ColumnText(3);
        document.revision = select.ColumnInt64(6);
        if (kind == DocumentKind::kNote) {
            document.style = select.ColumnText(4);
            document.detail = select.ColumnText(5);
        }
    }
    return document;
}

std::vector<asr::Turn> SqliteSessionStore::ReadTurns(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ChunkCipher cipher = CipherFor(id);
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
        audio.insert(audio.end(), frames, frames + plain.size() / sizeof(float));
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

// A failed commit keeps its audio for the next tick. Past kPendingBound the oldest frames are
// dropped as lost and the stored timeline moves with them. The fault is announced once per episode
void SqliteSessionStore::WriterLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        cv_.wait_for(lock, commit_interval_, [this] { return stopping_; });
        if (stopping_) break;
        if (!open_.has_value()) continue;
        try {
            if (CommitPending() && open_->faulted) {
                open_->faulted = false;
                std::fprintf(stderr, "clinicavt-engine: store commits again\n");
            }
        } catch (const StoreError& e) {
            Open& session = *open_;
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
