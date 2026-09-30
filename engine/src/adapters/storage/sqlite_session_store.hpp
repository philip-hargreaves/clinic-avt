#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

#include "adapters/storage/chunk_cipher.hpp"
#include "adapters/storage/db.hpp"
#include "core/common/worker_thread.hpp"
#include "ports/session_store.hpp"

namespace clinicavt::store {

// Stores sessions in clinicavt.db with each blob sealed under its session's key. A writer thread
// commits the audio once per interval. The tables are in schema/clinicavt.sql
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
    std::size_t DeleteAll(bool keep_reflections = false) override;
    void Clear(const SessionId& id) override;
    bool Cleared(const SessionId& id) override;
    SessionRecord ReadRecord(const SessionId& id) override;
    AddOutcome AddRecord(const SessionRecord& record) override;
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
    // Capture buffer, guarded only by pending_mutex_ so Append never waits on the DB
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
    // Writes one document row inside the caller's transaction
    void WriteDocumentRow(const SessionId& id, DocumentKind kind, const ChunkCipher& cipher,
                          std::int64_t seq, const Document& document);
    Document ReadDocumentLocked(const SessionId& id, DocumentKind kind);
    // nullopt if the session has no such document
    std::optional<Document> ReadDocumentRow(const SessionId& id, DocumentKind kind,
                                            const ChunkCipher& cipher);
    std::vector<asr::Turn> ReadTurnsLocked(const SessionId& id, const ChunkCipher& cipher);
    // AddRecord for an id already stored, inside its transaction
    AddOutcome CompleteLocked(const SessionRecord& record);
    void ClearLocked(const SessionId& id);    // Clear inside the caller's transaction
    void Erase(const SessionId& id);          // key row and everything under the session
    std::size_t EraseWhere(Db::Stmt& erase);  // steps a delete, checkpoints, counts
    void Checkpoint();                        // after an erase, so the WAL keeps no old page
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
    WorkerThread writer_;
    std::mutex pending_mutex_;  // lock order: mutex_ first
    Pending pending_;
};

}  // namespace clinicavt::store
