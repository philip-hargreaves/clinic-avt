#include <cstddef>
#include <mutex>
#include <vector>

#include "adapters/storage/sqlite_session_store.hpp"
#include "adapters/storage/sqlite_store_rows.hpp"
#include "core/common/log.hpp"

namespace clinicavt::store {

using rows::AsBytes;
using rows::kPendingBound;

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
    while (!writer_.Stopping()) {
        cv_.wait_for(lock, commit_interval_, [this] { return writer_.Stopping(); });
        if (writer_.Stopping()) break;
        if (!open_.has_value()) continue;
        Open& session = *open_;
        try {
            if (CommitPending() && session.faulted) {
                session.faulted = false;
                log::Printf("clinicavt-engine: store commits again\n");
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
                log::Printf("clinicavt-engine: store commit failed: %s\n", e.what());
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

}  // namespace clinicavt::store
