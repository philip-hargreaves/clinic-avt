#include "adapters/storage/sqlite_session_store.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

#include "adapters/storage/sqlite_store_rows.hpp"
#include "adapters/storage/store_migrations.hpp"
#include "core/common/iso8601.hpp"
#include "core/common/log.hpp"

namespace clinicavt::store {

using rows::AsBytes;
using rows::kSqlitePageLimit;
using rows::RandomId;

SqliteSessionStore::SqliteSessionStore(const std::filesystem::path& root,
                                       std::chrono::milliseconds commit_interval)
    : commit_interval_(commit_interval), db_(OpenDatabase(root)), writer_(mutex_, cv_) {
    writer_.Start([this] { WriterLoop(); });
}

SqliteSessionStore::~SqliteSessionStore() {
    writer_.Stop();
    // An open session is left recording, so it stays recoverable
    if (open_.has_value()) {
        try {
            CommitPending();
        } catch (const StoreError& e) {
            log::Printf("clinicavt-engine: store commit failed at close: %s\n", e.what());
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

    Db::Transaction txn(db_);
    Db::Stmt insert = db_.Prepare(
        "INSERT INTO consultations(id, started_at, state, sample_rate, device_id, device_name, "
        "saved)"
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

// Timings stay plaintext for the audio length in the list. Speaker and text are sealed
void SqliteSessionStore::InsertTurn(const SessionId& id, std::int64_t seq,
                                    const ChunkCipher& cipher, const asr::Turn& turn) {
    const std::string content =
        nlohmann::json{{"speaker", turn.speaker}, {"text", turn.text}}.dump();
    const std::vector<std::uint8_t> sealed =
        cipher.Seal(Domain::kTurns, id, static_cast<std::uint64_t>(seq), AsBytes(content));
    Db::Stmt insert = db_.Prepare(
        "INSERT INTO turns(consultation_id, sequence, first_frame, frame_count, encrypted_turn)"
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
    Db::Stmt erase = db_.Prepare("DELETE FROM turns WHERE consultation_id = ?");
    erase.BindText(1, session.id);
    erase.Step();
    for (const asr::Turn& turn : turns) {
        // Sequences continue past the replaced turns, so no IV is used twice
        InsertTurn(session.id, session.next_turn_seq, *session.cipher, turn);
        session.next_turn_seq += 1;
    }
    txn.Commit();
}

// The audio was kept only to resume after a crash, so it is erased here
void SqliteSessionStore::Finalise(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Open& session = RequireOpen(id);
    TakePending(session);
    const std::uint64_t lost = session.lost_committed + session.held_lost;

    Db::Transaction txn(db_);
    Db::Stmt erase = db_.Prepare("DELETE FROM audio_chunks WHERE consultation_id = ?");
    erase.BindText(1, session.id);
    erase.Step();
    Db::Stmt update = db_.Prepare(
        "UPDATE consultations SET ended_at = ?, state = 'finalised', dropped_frames = ?"
        " WHERE id = ?");
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
    // The row stays in the recording state, which lists it for recovery
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

// The key row goes with the session, so ciphertext left in free pages cannot be read
void SqliteSessionStore::Erase(const SessionId& id) {
    Db::Stmt erase = db_.Prepare("DELETE FROM consultations WHERE id = ?");
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
    Db::Stmt erase =
        db_.Prepare("DELETE FROM consultations WHERE saved = 0 AND state = 'finalised'");
    EraseWhere(erase);
}

void SqliteSessionStore::RequireStored(const SessionId& id) {
    if (open_.has_value() && open_->id == id) {
        throw StoreError(StoreCode::kBusy, id + " is still recording");
    }
    Db::Stmt select = db_.Prepare("SELECT 1 FROM consultation_keys WHERE consultation_id = ?");
    select.BindText(1, id);
    if (!select.Step()) throw StoreError(StoreCode::kNotFound, "no session " + id);
}

ChunkCipher SqliteSessionStore::CipherFor(const SessionId& id) {
    RequireStored(id);
    Db::Stmt select =
        db_.Prepare("SELECT wrapped_key FROM consultation_keys WHERE consultation_id = ?");
    select.BindText(1, id);
    select.Step();
    return ChunkCipher::FromWrapped(select.ColumnBlob(0));
}

void SqliteSessionStore::InsertKey(const SessionId& id, std::span<const std::uint8_t> wrapped) {
    Db::Stmt key =
        db_.Prepare("INSERT INTO consultation_keys(consultation_id, wrapped_key) VALUES(?, ?)");
    key.BindText(1, id);
    key.BindBlob(2, wrapped);
    key.Step();
}

std::vector<asr::Turn> SqliteSessionStore::ReadTurns(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ChunkCipher cipher = CipherFor(id);
    return ReadTurnsLocked(id, cipher);
}

std::vector<asr::Turn> SqliteSessionStore::ReadTurnsLocked(const SessionId& id,
                                                           const ChunkCipher& cipher) {
    Db::Stmt select = db_.Prepare(
        "SELECT sequence, first_frame, frame_count, encrypted_turn FROM turns"
        " WHERE consultation_id = ?"
        " ORDER BY sequence");
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
    Db::Stmt select = db_.Prepare(
        "SELECT sequence, encrypted_audio FROM audio_chunks WHERE consultation_id = ?"
        " ORDER BY sequence");
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
        log::Printf("clinicavt-engine: store log kept, a reader holds it\n");
    }
}

}  // namespace clinicavt::store
