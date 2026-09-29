#include "adapters/system/gpu_lease.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace clinicavt::system {
namespace {

using std::chrono::milliseconds;

// Each test's own name, so a wedged mark never reaches another test or the engine's lease
std::string Name(const char* what) {
    return std::string("Local\\clinicavt-gpu-lease-") + what + "-" +
           std::to_string(GetCurrentProcessId());
}

TEST(GpuLease, TwoHoldersOfOneNameTakeTurns) {
    const std::string name = Name("turns");
    GpuLease engine(name, milliseconds(50));
    GpuLease host(name, milliseconds(50));  // a second handle, as the other process would open it
    ASSERT_TRUE(engine.Active());
    ASSERT_TRUE(host.Active());

    std::atomic<bool> inside{false};
    std::atomic<bool> overlapped{false};
    auto first = engine.Acquire();
    std::thread other([&] {
        const auto guard = host.Acquire();
        overlapped = inside.load();
        EXPECT_TRUE(guard.Held());
        EXPECT_GT(guard.Waited(), 0.05);
    });
    inside = true;
    std::this_thread::sleep_for(milliseconds(200));
    inside = false;
    first = GpuLease::Guard{};  // release
    other.join();
    EXPECT_FALSE(overlapped) << "the second holder must wait for the first to release";
}

TEST(GpuLease, AWaiterGivesUpOnlyWhenItsCallbackSaysSo) {
    const std::string name = Name("slices");
    GpuLease holder(name, milliseconds(20));
    GpuLease waiter(name, milliseconds(20));
    const auto held = holder.Acquire();

    int slices = 0;
    GpuLease::Guard guard;
    // On another thread: a mutex is recursive for the thread that owns it
    std::thread other([&] { guard = waiter.Acquire([&](double) { return ++slices < 5; }); });
    other.join();

    EXPECT_FALSE(guard.Held());
    EXPECT_EQ(slices, 5);
    EXPECT_FALSE(waiter.Wedged()) << "giving up once is not a verdict on the GPU";
}

// Whichever engine finds a stuck host, every other one stops waiting on it too
TEST(GpuLease, AStuckMarkReachesEveryHolderAndEndsTheirWaits) {
    const std::string name = Name("wedged");
    GpuLease holder(name);
    GpuLease finder(name);
    GpuLease waiter(name);
    const auto held = holder.Acquire();

    finder.MarkWedged();

    EXPECT_TRUE(waiter.Wedged());
    const auto t0 = std::chrono::steady_clock::now();
    std::thread other([&] { EXPECT_FALSE(waiter.Acquire().Held()); });
    other.join();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, milliseconds(100)) << "never waits";
}

// A holder that died mid-work leaves the mutex abandoned. The next waiter
// takes it rather than waiting for ever
TEST(GpuLease, AnAbandonedLeaseIsTaken) {
    const std::string name = Name("abandoned");
    GpuLease next(name, milliseconds(50));
    std::thread([&] {
        HANDLE raw = CreateMutexExA(nullptr, name.c_str(), 0, SYNCHRONIZE);
        WaitForSingleObject(raw, INFINITE);
        CloseHandle(raw);  // the thread ends still owning it
    }).join();

    std::thread([&] { EXPECT_TRUE(next.Acquire().Held()); }).join();
}

// Prefill runs only if the GPU is free now; an unnamed lease is off
TEST(GpuLease, TryAcquireAndAnUnnamedLeaseNeverWait) {
    GpuLease off("");
    EXPECT_FALSE(off.Active());
    const auto inert = off.Acquire();
    EXPECT_FALSE(inert.Held());
    EXPECT_EQ(inert.Waited(), 0.0);

    const std::string name = Name("try");
    GpuLease holder(name);
    GpuLease prefill(name);
    std::thread([&] { EXPECT_TRUE(prefill.TryAcquire().Held()); }).join();
    const auto held = holder.Acquire();
    std::thread([&] { EXPECT_FALSE(prefill.TryAcquire().Held()); }).join();
}

// A host that is merely tearing down is seen once; a stuck one is still there
// on the next slice. Only the second sighting in a row gives the wait up
TEST(GpuLease, TheWatchWedgesTheLeaseOnlyForAnOrphanSeenOnTwoSlicesInARow) {
    GpuLease lease(Name("watch"));
    const std::vector<std::vector<DWORD>> scans = {{7}, {}, {7}, {8}, {8}};
    std::size_t next = 0;
    auto watch = WatchForStuckHosts("test", lease, [&] { return scans.at(next++); });

    EXPECT_TRUE(watch(5.0)) << "first sighting";
    EXPECT_TRUE(watch(10.0)) << "gone";
    EXPECT_TRUE(watch(15.0)) << "back, but not twice in a row";
    EXPECT_TRUE(watch(20.0)) << "another host's first sighting";
    EXPECT_FALSE(lease.Wedged());

    EXPECT_FALSE(watch(25.0)) << "the same host on two slices in a row";
    EXPECT_TRUE(lease.Wedged());
}

// Past the probe time the lane asks its own host once; a stuck one ends the wait
TEST(GpuLease, TheWatchAsksItsOwnHostOnceAfterTheProbeTime) {
    GpuLease lease(Name("probe"));
    int probes = 0;
    bool stuck = false;
    lease.SetStuckProbe([&] {
        ++probes;
        return stuck;
    });
    auto healthy = WatchForStuckHosts("test", lease, [] { return std::vector<DWORD>{}; });
    EXPECT_TRUE(healthy(kProbeAfterSeconds - 5));
    EXPECT_EQ(probes, 0) << "a load can hold the GPU this long";
    EXPECT_TRUE(healthy(kProbeAfterSeconds));
    EXPECT_TRUE(healthy(kProbeAfterSeconds + 5));
    EXPECT_EQ(probes, 1) << "asked once per wait";

    stuck = true;
    auto waiting = WatchForStuckHosts("test", lease, [] { return std::vector<DWORD>{}; });
    EXPECT_FALSE(waiting(kProbeAfterSeconds));
}

}  // namespace
}  // namespace clinicavt::system
