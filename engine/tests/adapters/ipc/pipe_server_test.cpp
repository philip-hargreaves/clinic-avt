#include "adapters/ipc/pipe_server.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "adapters/ipc/pipe_client.hpp"

namespace clinicavt::ipc {
namespace {

using namespace std::chrono_literals;

std::wstring PipeName(const char* what) {
    return L"\\\\.\\pipe\\LOCAL\\clinicavt-server-test-" +
           std::wstring(what, what + std::strlen(what)) + L"-" +
           std::to_wstring(GetCurrentProcessId());
}

void Connect(PipeClient& client, const std::wstring& name) {
    for (int attempt = 0; attempt < 200 && !client.Open(name); ++attempt) {
        std::this_thread::sleep_for(25ms);
    }
    ASSERT_TRUE(client.IsOpen());
}

// A shell that closes can come back to the same engine, and a dial that
// connects and leaves at once never costs the next one its turn
TEST(PipeServer, AClientThatLeavesEarlyNeverBlocksTheNext) {
    const auto name = PipeName("turns");
    PipeServer server(name);
    server.RegisterMethod("engine/echo", [](const json& params) { return params; });
    std::vector<bool> spoke;
    std::thread engine([&] {
        while (server.AwaitClient(1s) == PipeServer::Accept::kClient)
            spoke.push_back(server.Serve());
    });

    PipeClient first;
    Connect(first, name);
    first.Close();
    PipeClient second;
    Connect(second, name);
    ASSERT_TRUE(second.Write(EncodeFrame(
        R"({"jsonrpc":"2.0","id":1,"method":"engine/echo","params":{"said":"hello"}})")));
    std::optional<std::string> reply;
    for (int poll = 0; poll < 200 && !reply; ++poll) {
        second.Read();
        reply = second.NextFrame();
        if (!reply) std::this_thread::sleep_for(25ms);
    }
    second.Close();
    engine.join();

    ASSERT_TRUE(reply.has_value());
    EXPECT_NE(reply->find("hello"), std::string::npos);
    // Only the second spoke. An engine asked to exit is kept to it by a silent visitor
    EXPECT_EQ(std::count(spoke.begin(), spoke.end(), true), 1);
    EXPECT_TRUE(spoke.back());
}

// An engine already asked to exit waits at zero idle, and busy still holds it
TEST(PipeServer, AnIdleServerGivesUpOnlyOnceNothingKeepsItBusy) {
    PipeServer server(PipeName("idle"));
    const auto t0 = std::chrono::steady_clock::now();

    const auto accept =
        server.AwaitClient(0ms, [&] { return std::chrono::steady_clock::now() < t0 + 1500ms; });

    EXPECT_EQ(accept, PipeServer::Accept::kIdle);
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 1500ms);
}

}  // namespace
}  // namespace clinicavt::ipc
