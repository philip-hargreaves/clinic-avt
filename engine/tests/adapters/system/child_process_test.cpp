#include "adapters/system/child_process.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace clinicavt::system {
namespace {

// A quiet ping stands in for a child that keeps running
ChildProcess Ping(const wchar_t* count) {
    wchar_t folder[MAX_PATH]{};
    GetSystemDirectoryW(folder, MAX_PATH);
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    UniqueHandle null(
        CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
    return ChildProcess::Spawn(std::filesystem::path(folder) / "ping.exe",
                               std::wstring(L"-n ") + count + L" 127.0.0.1",
                               {.stdout_write = null.get()});
}

// A child inside a GPU call must never be killed, so End only ever waits
TEST(ChildProcess, EndWaitsAndNeverKills) {
    auto running = Ping(L"30");
    EXPECT_FALSE(running.End(100));
    EXPECT_TRUE(running.Alive()) << "held, not killed";
    running.Kill();
    EXPECT_TRUE(running.End(5000));
    EXPECT_EQ(running.Pid(), 0u);

    auto exiting = Ping(L"1");
    EXPECT_TRUE(exiting.End(10'000)) << "a child that exits on its own is released";
    EXPECT_EQ(exiting.Pid(), 0u);
}

}  // namespace
}  // namespace clinicavt::system
