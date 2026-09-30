#include "adapters/system/gpu_lease.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "adapters/system/exe_paths.hpp"
#include "core/common/log.hpp"

namespace clinicavt::system {

std::string InheritedGpuLeaseName() {
#pragma warning(suppress : 4996)
    const char* name = std::getenv(kGpuLeaseVariable);
    return name != nullptr ? name : "";
}

GpuLease::Guard::Guard(Guard&& other) noexcept
    : mutex_(std::exchange(other.mutex_, nullptr)), waited_(other.waited_) {}

GpuLease::Guard& GpuLease::Guard::operator=(Guard&& other) noexcept {
    if (this != &other) {
        Release();
        mutex_ = std::exchange(other.mutex_, nullptr);
        waited_ = other.waited_;
    }
    return *this;
}

GpuLease::Guard::~Guard() {
    Release();
}

void GpuLease::Guard::Release() {
    if (mutex_ != nullptr) {
        ReleaseMutex(mutex_);
        mutex_ = nullptr;
    }
}

GpuLease::GpuLease(const std::string& name, std::chrono::milliseconds slice) : slice_(slice) {
    if (name.empty()) return;
    mutex_ = CreateMutexExA(nullptr, name.c_str(), 0, SYNCHRONIZE);
    if (mutex_ == nullptr) {
        log::Printf("clinicavt: GPU lease %s unavailable (error %lu)\n", name.c_str(),
                    GetLastError());
    }
    // Held by every process, stuck ones too, so once set it lasts until a restart
    wedged_event_ = CreateEventExA(nullptr, (name + "-wedged").c_str(), CREATE_EVENT_MANUAL_RESET,
                                   SYNCHRONIZE | EVENT_MODIFY_STATE);
}

GpuLease::~GpuLease() {
    if (mutex_ != nullptr) CloseHandle(mutex_);
    if (wedged_event_ != nullptr) CloseHandle(wedged_event_);
}

GpuLease::Guard GpuLease::Acquire(const OnWait& on_wait) {
    if (mutex_ == nullptr || Wedged()) return {};
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const DWORD result = WaitForSingleObject(mutex_, static_cast<DWORD>(slice_.count()));
        const double waited =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (result == WAIT_OBJECT_0) return {mutex_, waited};
        if (result == WAIT_ABANDONED) {
            log::Printf("clinicavt: the last GPU holder exited mid-work\n");
            return {mutex_, waited};
        }
        if (result != WAIT_TIMEOUT) {
            log::Printf("clinicavt: GPU lease wait failed (error %lu)\n", GetLastError());
            return {};
        }
        if (Wedged() || (on_wait && !on_wait(waited))) return {};
    }
}

GpuLease::Guard GpuLease::TryAcquire() {
    if (mutex_ == nullptr || Wedged()) return {};
    const DWORD result = WaitForSingleObject(mutex_, 0);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return {mutex_, 0.0};
    return {};
}

bool GpuLease::Wedged() const {
    return wedged_.load() ||
           (wedged_event_ != nullptr && WaitForSingleObject(wedged_event_, 0) == WAIT_OBJECT_0);
}

void GpuLease::MarkWedged() {
    wedged_ = true;
    if (wedged_event_ != nullptr) SetEvent(wedged_event_);
}

void GpuLease::SetStuckProbe(StuckProbe probe) {
    std::lock_guard<std::mutex> lock(probe_mutex_);
    probe_ = std::move(probe);
}

bool GpuLease::ProbeOwnHost() {
    StuckProbe probe;
    {
        std::lock_guard<std::mutex> lock(probe_mutex_);
        probe = probe_;
    }
    return probe && probe();
}

std::vector<ProcessId> OrphanedNoteHosts() {
    return OrphanedProcesses(kNoteHostExe);
}

GpuLease::OnWait WatchForStuckHosts(const char* who, GpuLease& lease, OrphanScan scan) {
    return [who, &lease, scan = std::move(scan), logged = 0.0, probed = false,
            seen = std::vector<ProcessId>{}](double waited) mutable {
        const auto orphans = scan();
        for (ProcessId pid : orphans) {
            if (std::find(seen.begin(), seen.end(), pid) == seen.end()) continue;
            log::Printf("clinicavt: %s found stuck note host %lu holding the GPU\n", who, pid);
            lease.MarkWedged();
            return false;
        }
        seen = orphans;
        // Only a load holds the GPU this long legitimately, and loads are not probed. Ask our own
        // host to exit, which a healthy one does
        if (!probed && waited >= kProbeAfterSeconds) {
            probed = true;
            if (lease.ProbeOwnHost()) {
                log::Printf("clinicavt: %s found its own note host stuck\n", who);
                return false;
            }
        }
        if (waited - logged >= 30.0) {
            logged = waited;
            log::Printf("clinicavt: %s waiting %.0f s for the GPU\n", who, waited);
        }
        return true;
    };
}

}  // namespace clinicavt::system
