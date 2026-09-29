#include "adapters/ipc/wire_events.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "adapters/ipc/pipe_client.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "handler_test_support.hpp"

namespace clinicavt::ipc {
namespace {

using namespace handler_test;

TEST(Handlers, AStorageFaultGoesOutAsTheFixture) {
    SessionStoreFixture fixture;
    const std::wstring name =
        L"\\\\.\\pipe\\LOCAL\\clinicavt-handlers-fault-" + std::to_wstring(GetCurrentProcessId());
    PipeServer server(name);
    WireEvents events(server, *fixture.store);
    PipeClient shell;
    ASSERT_TRUE(shell.Open(name));
    ASSERT_EQ(server.AwaitClient(std::chrono::seconds(5)), PipeServer::Accept::kClient);

    events.OnStorageFault("audio commit: database or disk is full");

    std::optional<std::string> frame;
    for (int poll = 0; poll < 200 && !frame; ++poll) {
        shell.Read();
        frame = shell.NextFrame();
        if (!frame) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(json::parse(*frame), LoadFixture("storage-fault.json"));
}

TEST(Handlers, EnrolmentNoteSheetAndSummaryEventsGoOutAsTheFixtures) {
    SessionStoreFixture fixture;
    WireShell shell;
    WireEvents events(shell.server, *fixture.store);
    ASSERT_NO_FATAL_FAILURE(shell.Connect());

    events.OnEnrolProgress({12.4, 9.1, {0.5F, false}});
    events.OnEnrolDone(true, "", 31.5);
    events.OnNoteRefused("This does not sound like a consultation.", true);
    events.OnNoteFailed("the note model stopped responding");
    events.OnPatientFailed("the note model stopped responding");
    events.OnSummaryFailed("a1b2c3d4e5f60718293a4b5c6d7e8f90", "no stored note");
    for (const char* name :
         {"anchor-progress.json", "anchor-enrolled.json", "note-refused.json", "note-failed.json",
          "patient-failed.json", "reflection-summaryFailed.json"}) {
        SCOPED_TRACE(name);
        const json expected = LoadFixture(name);
        const auto sent = shell.Notification(expected["method"].get<std::string>());
        ASSERT_TRUE(sent.has_value());
        EXPECT_EQ(*sent, expected);
    }
}

// The token rate depends on timing, so the rate fields are compared by name only
void ExpectStream(WireShell& shell, const char* fixture_name) {
    SCOPED_TRACE(fixture_name);
    const json expected = LoadFixture(fixture_name);
    const auto sent = shell.Notification(expected["method"].get<std::string>());
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(KeysOf((*sent)["params"]), KeysOf(expected["params"]));
    json params = (*sent)["params"];
    params.erase("tokensPerSecond");
    json want = expected["params"];
    want.erase("tokensPerSecond");
    EXPECT_EQ(params, want);
}

TEST(Handlers, StreamedNoteAndSheetGoOutAsTheFixtures) {
    SessionStoreFixture fixture;
    WireShell shell;
    WireEvents events(shell.server, *fixture.store);
    ASSERT_NO_FATAL_FAILURE(shell.Connect());

    // Parts are more than the 80 ms cap apart, so each is sent and the end message carries a rate
    events.OnNotePartial("Swollen left elbow");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    events.OnNotePartial("Swollen left elbow for a week.");
    events.OnNoteReady("Swollen left elbow for a week. No injury.");
    events.OnPatientPartial("Rest the elbow");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    events.OnPatientPartial("Rest the elbow and take ibuprofen");
    events.OnPatientReady("Rest the elbow and take ibuprofen with food.");

    ExpectStream(shell, "note-partial.json");
    ExpectStream(shell, "note-ready.json");
    ExpectStream(shell, "patient-partial.json");
    ExpectStream(shell, "patient-ready.json");
}

TEST(Handlers, TranslationGoesOutAsTheFixtures) {
    SessionStoreFixture fixture;
    WireShell shell;
    WireEvents events(shell.server, *fixture.store);
    FakeTranslator translator;
    clinicavt::translate::TranslateLane lane(
        translator, [&events](const std::string& method, const json& params) {
            events.OnTranslation(method, params);
        });
    ASSERT_NO_FATAL_FAILURE(shell.Connect());

    ASSERT_TRUE(lane.Run("Rest the elbow and take ibuprofen with food.", "Polish"));
    ExpectStream(shell, "translate-partial.json");
    ExpectStream(shell, "translate-ready.json");

    translator.fail = true;
    ASSERT_TRUE(WaitUntil([&] { return lane.Run("Rest the elbow.", "Polish"); }));
    const json failed = LoadFixture("translate-failed.json");
    const auto sent = shell.Notification("translate/failed");
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(*sent, failed);
}

}  // namespace
}  // namespace clinicavt::ipc
