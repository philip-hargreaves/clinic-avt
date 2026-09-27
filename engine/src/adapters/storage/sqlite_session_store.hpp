#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "adapters/storage/chunk_cipher.hpp"
#include "adapters/storage/db.hpp"
#include "ports/session_store.hpp"

namespace clinicavt::store {

// One clinicavt.db, content sealed per blob under per-session keys. A writer
// thread commits per interval. Layout in schema/clinicavt.sql
class SqliteSessionStore : public ISessionStore {
   public:
    explicit SqliteSessionStore(
        const std::filesystem::path& root,
        std::chrono::milliseconds commit_interval = std::chrono::seconds(1));
    ~SqliteSessionStore() override;

    SessionId Begin(const SessionMeta& meta) override;
    void Append(const SessionId& id, std::span<const float> frames,
                std::uint64_t lost_frames) override;
    void ReplaceTurns(const SessionId& id, std::span<const asr::Turn> turns) override;
    void Finalise(const SessionId& id) override;
    void Cancel(const SessionId& id) override;
    void Abandon(const SessionId& id) override;
    std::vector<SessionSummary> ListSessions() override;
    void SaveDocument(const SessionId& id, DocumentKind kind, const Document& document) override;
    void EditDocument(const SessionId& id, DocumentKind kind, const std::string& text) override;
    Document ReadDocument(const SessionId& id, DocumentKind kind) override;
    void DeleteDocument(const SessionId& id, DocumentKind kind) override;
    std::vector<asr::Turn> ReadTurns(const SessionId& id) override;
    std::vector<float> ReadAudio(const SessionId& id) override;
    void Delete(const SessionId& id) override;
    void EraseUnretained() override;
    SessionId Seed(const SessionSeed& seed) override;
    std::size_t ClearDemo() override;
    std::size_t DeleteAll() override;
    void SetFaultListener(std::function<void(const StoreError&)> listener) override;

    // Test hook: a small cap makes the next commit fail with a full disk. 0 lifts it
    void SetMaxPageCount(std::int64_t pages);

   private:
    struct Open {
        SessionId id;
        std::optional<ChunkCipher> cipher;
        std::uint64_t sample_rate = 0;
        std::int64_t next_seq = 0;
        std::int64_t next_turn_seq = 0;
        std::uint64_t frames_committed = 0;
        std::uint64_t lost_committed = 0;
        std::vector<float> held;  // taken from the capture buffer, awaiting commit
        std::uint64_t held_lost = 0;
        bool faulted = false;  // a commit failed and the listener was told
    };
    // The capture thread's buffer, under pending_mutex_ alone: Append never waits on the database
    struct Pending {
        SessionId id;
        std::vector<float> frames;
        std::uint64_t lost = 0;
    };

    // All private members expect mutex_ held
    Open& RequireOpen(const SessionId& id);
    void RequireStored(const SessionId& id);     // has a key row and is no longer recording
    ChunkCipher CipherFor(const SessionId& id);  // a stored session's key
    void InsertKey(const SessionId& id, std::span<const std::uint8_t> wrapped);
    void InsertTurn(const SessionId& id, std::int64_t seq, const ChunkCipher& cipher,
                    const asr::Turn& turn);
    void WriteDocument(const SessionId& id, DocumentKind kind, const Document& document);
    Document ReadDocumentLocked(const SessionId& id, DocumentKind kind);
    void Erase(const SessionId& id);          // key row and everything under the session
    std::size_t EraseWhere(Db::Stmt& erase);  // steps a delete, checkpoints, counts
    void Checkpoint();                        // after an erase, so no page image outlives it
    void TakePending(Open& session);          // moves the capture buffer into held
    void ClosePending();                      // no session accepts audio
    bool CommitPending();                     // seals held as one chunk, false when empty
    void WriterLoop();

    std::chrono::milliseconds commit_interval_;
    Db db_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Open> open_;
    std::function<void(const StoreError&)> on_fault_;
    bool stopping_ = false;
    std::thread writer_;
    std::mutex pending_mutex_;  // lock order: mutex_ first
    Pending pending_;
};

}  // namespace clinicavt::store
