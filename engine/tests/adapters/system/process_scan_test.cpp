#include "adapters/system/process_scan.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace clinicavt::system {
namespace {

// A note host outlives its engine only when stuck, so an orphan is one whose
// parent is gone or whose parent pid now belongs to a newer process
TEST(ProcessScan, AnOrphanIsAHostWhoseParentIsGoneOrNewer) {
    const std::vector<ProcessEntry> entries = {
        {10, 1, L"clinicavt_engine.exe", 100},
        {11, 10, L"clinicavt_note_host.exe", 200},    // its engine is alive
        {21, 20, L"clinicavt_note_host.exe", 300},    // pid 20 is gone
        {30, 1, L"explorer.exe", 900},                // took pid 30 after the host began
        {31, 30, L"clinicavt_note_host.exe", 400},    // so its engine is gone too
        {41, 40, L"clinicavt_ingest_host.exe", 500},  // not the image asked about
        {50, 1, L"clinicavt_engine.exe", 0},          // creation time unreadable
        {51, 50, L"clinicavt_note_host.exe", 600},    // so its parent counts as alive
    };

    // A false orphan would wedge the GPU lease until a restart
    EXPECT_EQ(Orphans(entries, L"clinicavt_note_host.exe"), (std::vector<DWORD>{21, 31}));
}

TEST(ProcessScan, ReturnsAtOnceWhenNoOrphanRuns) {
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(
        LingeringOrphans(L"clinicavt_no_such_process.exe", std::chrono::seconds(5)).empty());
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(1));
}

}  // namespace
}  // namespace clinicavt::system
