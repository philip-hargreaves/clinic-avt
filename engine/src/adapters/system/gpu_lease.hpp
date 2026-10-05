#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "adapters/system/process_scan.hpp"

namespace clinicavt::system {

// One name per logon session so all engines and note hosts share the lease
inline constexpr const char* kGpuLeaseName = "Local\\clinicavt-gpu";

// Set by the engine and inherited by the hosts it starts
inline constexpr const char* kGpuLeaseVariable = "CLINICAVT_GPU_LEASE";

// The lease name inherited from the engine, empty when unset
std::string InheritedGpuLeaseName();

// Named mutex shared by all engines and note hosts, so two models never use the GPU at once
class GpuLease {
   public:
    class Guard {
       public:
        Guard() = default;
        Guard(void* mutex, double waited) : mutex_(mutex), waited_(waited) {}
        Guard(Guard&& other) noexcept;
        Guard& operator=(Guard&& other) noexcept;
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard();

        bool Held() const {
            return mutex_ != nullptr;
        }

        // Seconds spent waiting for another holder
        double Waited() const {
            return waited_;
        }

       private:
        void Release();
        void* mutex_ = nullptr;  // HANDLE
        double waited_ = 0.0;
    };

    // Called periodically while waiting, with seconds waited. Return false to give up
    using OnWait = std::function<bool(double)>;

    explicit GpuLease(const std::string& name, std::chrono::milliseconds slice = kSlice);
    ~GpuLease();
    GpuLease(const GpuLease&) = delete;
    GpuLease& operator=(const GpuLease&) = delete;

    bool Active() const {
        return mutex_ != nullptr;
    }

    // Checked every kPoll while waiting. True gives up at once
    using GiveUp = std::function<bool()>;

    // Blocks until acquired or a callback gives up
    Guard Acquire(const OnWait& on_wait = {}, const GiveUp& give_up = {});

    // Non-blocking acquire, for work only worth doing immediately
    Guard TryAcquire();

    // Set once any process sharing the name finds a GPU holder stuck in the driver.
    // Later acquires return immediately since the wait would never end
    bool Wedged() const;

    void MarkWedged();

    // Returns true if this process's own note host is the stuck holder. Set by the engine
    using StuckProbe = std::function<bool()>;
    void SetStuckProbe(StuckProbe probe);
    bool ProbeOwnHost();

   private:
    static constexpr std::chrono::milliseconds kSlice{5000};
    static constexpr std::chrono::milliseconds kPoll{100};
    void* mutex_ = nullptr;         // HANDLE
    void* wedged_event_ = nullptr;  // HANDLE
    std::chrono::milliseconds slice_;
    std::atomic<bool> wedged_{false};
    std::mutex probe_mutex_;
    StuckProbe probe_;
};

inline constexpr double kProbeAfterSeconds = 180.0;

// pids of note hosts with no engine
using OrphanScan = std::function<std::vector<ProcessId>()>;

std::vector<ProcessId> OrphanedNoteHosts();

// Default wait: logs every 30 s and gives up for good if an orphaned note host
// is still there one slice later, since only a stuck host lingers. Tests
// inject the lease and scan
GpuLease::OnWait WatchForStuckHosts(const char* who, GpuLease& lease,
                                    OrphanScan scan = OrphanedNoteHosts);

}  // namespace clinicavt::system
