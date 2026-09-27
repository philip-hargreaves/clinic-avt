#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>

#include "adapters/archive/archive_file.hpp"
#include "core/archive/backup.hpp"
#include "ports/session_store.hpp"

namespace clinicavt::archive {

// The notifications a job sends. job is "backup" or "restore"
nlohmann::json ArchiveProgressJson(const std::string& job, Phase phase, std::size_t done,
                                   std::size_t total);
nlohmann::json BackupDoneJson(const BackupResult& result);
nlohmann::json RestoreDoneJson(const RestoreResult& result, bool dry_run);
nlohmann::json ArchiveFailedJson(const std::string& job, const std::string& code);

// Runs one backup or restore at a time off the RPC thread, announcing archive/progress, then
// archive/done or archive/failed with a fixed code. The key derivation alone takes about a second
class ArchiveLane {
   public:
    using Emit = std::function<void(const std::string& method, nlohmann::json params)>;

    // The iteration count is only lowered by tests
    ArchiveLane(store::ISessionStore& store, Emit emit,
                std::uint32_t iterations = kBackupIterations);
    ~ArchiveLane();

    ArchiveLane(const ArchiveLane&) = delete;
    ArchiveLane& operator=(const ArchiveLane&) = delete;

    // Cleared before the final notification goes out, so a reply to it is never refused as busy
    bool Busy() const {
        return running_.load();
    }

    // False when a job is already running. The lane's copy of the password is wiped once the key
    // is derived
    bool BackUp(Period period, std::filesystem::path path, const std::string& password);
    bool Restore(std::filesystem::path path, const std::string& password, bool dry_run);

   private:
    struct Outcome {
        std::string method;
        nlohmann::json params;
    };

    bool Launch(std::function<Outcome()> job);
    Progress Reporter(std::string job);

    store::ISessionStore& store_;
    Emit emit_;
    std::uint32_t iterations_;
    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> running_{false};
};

}  // namespace clinicavt::archive
