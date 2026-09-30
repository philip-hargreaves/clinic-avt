#include "adapters/storage/sqlite_session_store.hpp"
#include "adapters/storage/sqlite_store_rows.hpp"

namespace clinicavt::store {

using rows::RandomId;

SessionId SqliteSessionStore::Seed(const SessionSeed& seed) {
    std::lock_guard<std::mutex> lock(mutex_);
    const SessionId id = RandomId();
    const ChunkCipher cipher = ChunkCipher::Generate();

    Db::Transaction txn(db_);
    Db::Stmt insert = db_.Prepare(
        "INSERT INTO consultations(id, started_at, ended_at, state, sample_rate, saved, sample)"
        " VALUES(?, ?, ?, 'finalised', ?, 1, 1)");
    insert.BindText(1, id);
    insert.BindText(2, seed.started_at);
    insert.BindTextOrNull(3, seed.ended_at);
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

std::size_t SqliteSessionStore::ClearDemo() {
    std::lock_guard<std::mutex> lock(mutex_);
    Db::Stmt erase = db_.Prepare("DELETE FROM consultations WHERE sample = 1");
    return EraseWhere(erase);
}

}  // namespace clinicavt::store
