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

// One name for the whole logon session, so every engine and note host on the
// machine takes turns, whichever engine started it
inline constexpr const char* kGpuLeaseName = "Local\\clinicavt-gpu";

// One named mutex serialises every GPU call of the engines and note hosts, so
// two models never run concurrently (the driver fault configuration). Named
// by CLINICAVT_GPU_LEASE, which the engine sets and its hosts inherit. Inert
// when unset
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

        // Seconds spent waiting for another holder to finish
        double waited() const {
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

    // Runs each time a slice of waiting passes, with the seconds waited so
    // far. False gives the wait up
    using OnWait = std::function<bool(double)>;

    explicit GpuLease(const std::string& name, std::chrono::milliseconds slice = kSlice)
        : slice_(slice) {
        if (name.empty()) return;
        mutex_ = CreateMutexExA(nullptr, name.c_str(), 0, SYNCHRONIZE);
        if (mutex_ == nullptr) {
            std::fprintf(stderr, "clinicavt: GPU lease %s unavailable (error %lu)\n", name.c_str(),
                         GetLastError());
        }
        // Every process holds it, a stuck one included, so once set it lasts until a restart
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

    // Blocks until the GPU is ours, or `on_wait` gives up, or the lease is
    // found wedged. An abandoned mutex counts as acquired
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

    // The GPU if it is free now. For work that is only worth doing at once
    Guard TryAcquire() {
        if (mutex_ == nullptr || Wedged()) return {};
        const DWORD result = WaitForSingleObject(mutex_, 0);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return {mutex_, 0.0};
        return {};
    }

    // Set once a process holding the GPU is known to be stuck in the driver, by
    // any engine or host sharing the name. Every later acquire returns at once,
    // since the wait could never end
    bool Wedged() const {
        return wedged_.load() ||
               (wedged_event_ != nullptr && WaitForSingleObject(wedged_event_, 0) == WAIT_OBJECT_0);
    }

    void MarkWedged() {
        wedged_ = true;
        if (wedged_event_ != nullptr) SetEvent(wedged_event_);
    }

    // Asks whether this process's own note host is the stuck holder. True
    // when it is. Set by the engine, which owns the host
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

// The note hosts whose engine has gone
using OrphanScan = std::function<std::vector<DWORD>()>;

inline std::vector<DWORD> OrphanedNoteHosts() {
    return OrphanedProcesses(kNoteHostExe);
}

// The usual wait: logged every half minute, and given up for good when a
// note host whose engine has gone is still there a slice later, since only a
// stuck host lingers. Tests pass their own lease and scan
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
        // Nothing legitimate but a load holds the GPU this long, and a load is
        // never probed. The own host is asked to exit: a healthy one does
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
