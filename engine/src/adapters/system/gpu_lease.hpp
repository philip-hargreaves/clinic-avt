#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "adapters/system/exe_paths.hpp"
#include "adapters/system/process_scan.hpp"

namespace clinicavt::system {

// One name per logon session so all engines and note hosts share the lease
inline constexpr const char* kGpuLeaseName = "Local\\clinicavt-gpu";

// Named mutex serialising all GPU work of engines and note hosts, so two models
// never run at once (the driver-fault configuration). Name comes from
// CLINICAVT_GPU_LEASE, set by the engine and inherited by hosts. Inert when unset
class GpuLease {
   public:
    class Guard {
       public:
        Guard() = default;
        Guard(HANDLE mutex, double waited) : mutex_(mutex), waited_(waited) {}
        Guard(Guard&& other) noexcept
            : mutex_(std::exchange(other.mutex_, nullptr)), waited_(other.waited_) {}
        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                Release();
                mutex_ = std::exchange(other.mutex_, nullptr);
                waited_ = other.waited_;
            }
            return *this;
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() {
            Release();
        }

        bool Held() const {
            return mutex_ != nullptr;
        }

        // Seconds spent waiting for another holder
        double Waited() const {
            return waited_;
        }

       private:
        void Release() {
            if (mutex_ != nullptr) {
                ReleaseMutex(mutex_);
                mutex_ = nullptr;
            }
        }
        HANDLE mutex_ = nullptr;
        double waited_ = 0.0;
    };

    // Called periodically while waiting, with seconds waited. Return false to give up
    using OnWait = std::function<bool(double)>;

    explicit GpuLease(const std::string& name, std::chrono::milliseconds slice = kSlice)
        : slice_(slice) {
        if (name.empty()) return;
        mutex_ = CreateMutexExA(nullptr, name.c_str(), 0, SYNCHRONIZE);
        if (mutex_ == nullptr) {
            std::fprintf(stderr, "clinicavt: GPU lease %s unavailable (error %lu)\n", name.c_str(),
                         GetLastError());
        }
        // Held by every process, stuck ones too, so once set it lasts until a restart
        wedged_event_ = CreateEventExA(nullptr, (name + "-wedged").c_str(),
                                       CREATE_EVENT_MANUAL_RESET, SYNCHRONIZE | EVENT_MODIFY_STATE);
    }
    ~GpuLease() {
        if (mutex_ != nullptr) CloseHandle(mutex_);
        if (wedged_event_ != nullptr) CloseHandle(wedged_event_);
    }
    GpuLease(const GpuLease&) = delete;
    GpuLease& operator=(const GpuLease&) = delete;

    static GpuLease& Global() {
        static GpuLease lease([] {
#pragma warning(suppress : 4996)
            const char* name = std::getenv("CLINICAVT_GPU_LEASE");
            return std::string(name != nullptr ? name : "");
        }());
        return lease;
    }

    bool Active() const {
        return mutex_ != nullptr;
    }

    // Blocks until acquired, `on_wait` gives up, or the lease is wedged. An
    // abandoned mutex counts as acquired
    Guard Acquire(const OnWait& on_wait = {}) {
        if (mutex_ == nullptr || Wedged()) return {};
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            const DWORD result = WaitForSingleObject(mutex_, static_cast<DWORD>(slice_.count()));
            const double waited =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (result == WAIT_OBJECT_0) return {mutex_, waited};
            if (result == WAIT_ABANDONED) {
                std::fprintf(stderr, "clinicavt: the last GPU holder exited mid-work\n");
                return {mutex_, waited};
            }
            if (result != WAIT_TIMEOUT) {
                std::fprintf(stderr, "clinicavt: GPU lease wait failed (error %lu)\n",
                             GetLastError());
                return {};
            }
            if (Wedged() || (on_wait && !on_wait(waited))) return {};
        }
    }

    // Non-blocking acquire, for work only worth doing immediately
    Guard TryAcquire() {
        if (mutex_ == nullptr || Wedged()) return {};
        const DWORD result = WaitForSingleObject(mutex_, 0);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return {mutex_, 0.0};
        return {};
    }

    // Set once any process sharing the name finds a GPU holder stuck in the driver.
    // Later acquires return immediately since the wait would never end
    bool Wedged() const {
        return wedged_.load() ||
               (wedged_event_ != nullptr && WaitForSingleObject(wedged_event_, 0) == WAIT_OBJECT_0);
    }

    void MarkWedged() {
        wedged_ = true;
        if (wedged_event_ != nullptr) SetEvent(wedged_event_);
    }

    // Returns true if this process's own note host is the stuck holder. Set by the engine
    using StuckProbe = std::function<bool()>;
    void SetStuckProbe(StuckProbe probe) {
        std::lock_guard<std::mutex> lock(probe_mutex_);
        probe_ = std::move(probe);
    }
    bool ProbeOwnHost() {
        StuckProbe probe;
        {
            std::lock_guard<std::mutex> lock(probe_mutex_);
            probe = probe_;
        }
        return probe && probe();
    }

   private:
    static constexpr std::chrono::milliseconds kSlice{5000};
    HANDLE mutex_ = nullptr;
    HANDLE wedged_event_ = nullptr;
    std::chrono::milliseconds slice_;
    std::atomic<bool> wedged_{false};
    std::mutex probe_mutex_;
    StuckProbe probe_;
};

inline constexpr double kProbeAfterSeconds = 180.0;

// pids of note hosts with no engine
using OrphanScan = std::function<std::vector<DWORD>()>;

inline std::vector<DWORD> OrphanedNoteHosts() {
    return OrphanedProcesses(kNoteHostExe);
}

// Default wait: logs every 30 s and gives up for good if an orphaned note host
// is still there one slice later, since only a stuck host lingers. Tests
// inject the lease and scan
inline GpuLease::OnWait WatchForStuckHosts(const char* who, GpuLease& lease = GpuLease::Global(),
                                           OrphanScan scan = OrphanedNoteHosts) {
    return [who, &lease, scan = std::move(scan), logged = 0.0, probed = false,
            seen = std::vector<DWORD>{}](double waited) mutable {
        const auto orphans = scan();
        for (DWORD pid : orphans) {
            if (std::find(seen.begin(), seen.end(), pid) == seen.end()) continue;
            std::fprintf(stderr, "clinicavt: %s found stuck note host %lu holding the GPU\n", who,
                         pid);
            lease.MarkWedged();
            return false;
        }
        seen = orphans;
        // Only a load holds the GPU this long legitimately, and loads are not probed.
        // Ask our own host to exit; a healthy one will
        if (!probed && waited >= kProbeAfterSeconds) {
            probed = true;
            if (lease.ProbeOwnHost()) {
                std::fprintf(stderr, "clinicavt: %s found its own note host stuck\n", who);
                return false;
            }
        }
        if (waited - logged >= 30.0) {
            logged = waited;
            std::fprintf(stderr, "clinicavt: %s waiting %.0f s for the GPU\n", who, waited);
        }
        return true;
    };
}

}  // namespace clinicavt::system
