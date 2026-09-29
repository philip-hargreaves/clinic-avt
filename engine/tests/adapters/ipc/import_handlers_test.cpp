#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "adapters/ipc/handlers.hpp"
#include "handler_test_support.hpp"

namespace clinicavt::ipc {
namespace {

using namespace handler_test;

TEST(Handlers, RecordingInspectAndSessionImportMatchTheFixtures) {
    using namespace std::chrono;
    ImportRig rig;
    const json inspect = LoadFixture("recording-inspect.json");
    rig.reader.info = {760.4, sys_days{2026y / 9 / 26} + 13h + 5min};
    const auto inspected = HandleRecordingInspect(rig.reader, inspect["request"]["params"]);
    ASSERT_TRUE(std::holds_alternative<json>(inspected));
    EXPECT_EQ(MakeResult(std::int64_t{36}, ResultOf(inspected)), inspect["response"]);
    (void)HandleRecordingInspect(rig.reader, json{{"path", "C:\\Visite \u00e0 domicile.m4a"}});
    EXPECT_EQ(rig.reader.last_path.wstring(), L"C:\\Visite \u00e0 domicile.m4a")
        << "the wire's UTF-8 names the file";

    const json import = LoadFixture("session-import.json");
    rig.reader.audio.assign(2 * 16000, 0.1F);
    const auto imported = rig.Import(import["request"]["params"]);
    ASSERT_TRUE(std::holds_alternative<json>(imported));
    const json& result = ResultOf(imported);
    EXPECT_EQ(result.size(), import["response"]["result"].size());
    ASSERT_TRUE(result["sessionId"].is_string());

    const auto end = rig.WaitForEnd();
    ASSERT_TRUE(end.has_value());
    const json done = LoadFixture("session-imported.json");
    EXPECT_EQ(end->first, done["method"].get<std::string>());
    EXPECT_EQ(end->second, (json{{"sessionId", result["sessionId"]}})) << "the session answered";
    EXPECT_EQ(end->second.size(), done["params"].size());
    EXPECT_EQ(rig.controller.LastFinalised(), result["sessionId"].get<std::string>())
        << "sealed as a stop seals";
    ASSERT_TRUE(WaitUntil([&] { return rig.Methods().size() == 3; }));
    EXPECT_EQ(rig.Methods(),
              (std::vector<std::string>{"session/imported", "note/ready", "patient/ready"}))
        << "without a writer the stubs keep the contract, after the seal";
    EXPECT_EQ(rig.Progress(),
              (std::vector<std::string>{"reading 5", "finalising 95", "finalising 100"}))
        << "the scripted diariser finds and decodes nothing, so those stages say nothing";

    const json list = HandleSessionList(rig.fixture.records);
    ASSERT_EQ(list["sessions"].size(), 1u);
    EXPECT_EQ(list["sessions"][0]["id"], result["sessionId"]);
    EXPECT_EQ(list["sessions"][0]["startedAt"], "2026-09-26T13:05:00Z") << "dated as chosen";
    EXPECT_FALSE(list["sessions"][0]["endedAt"].get<std::string>().empty());

    const json progress = LoadFixture("session-importProgress.json");
    EXPECT_EQ(ImportProgressJson("a1b2c3d4e5f60718293a4b5c6d7e8f90",
                                 clinicavt::session::ImportStage::kTranscribing, 60),
              progress["params"]);
}

TEST(Handlers, ACancelledImportEndsAsAFailureNamedCancelledAndLeavesNothing) {
    ImportRig rig;
    rig.reader.audio.assign(2 * 16000, 0.1F);
    std::atomic<bool> decoding{false};
    std::atomic<bool> release{false};
    rig.reader.fail_decode = [&] {
        decoding = true;
        (void)WaitUntil([&] { return release.load(); });
    };
    const auto imported =
        rig.Import(json{{"path", "C:\\visit.m4a"}, {"startedAt", "2026-09-26T13:05:00Z"}});
    ASSERT_TRUE(std::holds_alternative<json>(imported));
    ASSERT_TRUE(WaitUntil([&] { return decoding.load(); }));
    rig.controller.Cancel();
    release = true;

    const auto end = rig.WaitForEnd();
    ASSERT_TRUE(end.has_value());
    EXPECT_EQ(end->first, "session/importFailed");
    const json failed = LoadFixture("session-importFailed.json");
    EXPECT_EQ(end->second, (json{{"sessionId", ResultOf(imported)["sessionId"]},
                                 {"error", failed["params"]["error"]}}));
    EXPECT_TRUE(HandleSessionList(rig.fixture.records)["sessions"].empty());
    EXPECT_FALSE(rig.controller.Running());
    EXPECT_EQ(rig.Methods(), (std::vector<std::string>{"session/importFailed"}))
        << "no stubs, since no note follows";
}

TEST(Handlers, SessionImportRefusesBeforeReadingAnything) {
    ImportRig rig;
    const json good{{"path", "C:\\visit.m4a"}, {"startedAt", "2026-09-26T13:05:00Z"}};
    const auto with = [&](const char* key, json value) {
        json params = good;
        params[key] = std::move(value);
        return params;
    };
    struct Row {
        const char* name;
        json params;
        int code;
    };
    const Row rows[] = {
        {"no path", json{{"startedAt", "2026-09-26T13:05:00Z"}}, kInvalidParams},
        {"no date", json{{"path", "C:\\visit.m4a"}}, kInvalidParams},
        {"local time", with("startedAt", "2026-09-26T14:05:00+01:00"), kInvalidParams},
        {"fractions", with("startedAt", "2026-09-26T13:05:00.000Z"), kInvalidParams},
        {"no time", with("startedAt", "2026-09-26"), kInvalidParams},
        {"retain as text", with("retain", "yes"), kInvalidParams},
        {"the future", with("startedAt", "2999-01-01T00:00:00Z"), kSessionError},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        const auto outcome = rig.Import(row.params);
        ASSERT_TRUE(std::holds_alternative<Error>(outcome));
        EXPECT_EQ(std::get<Error>(outcome).code, row.code);
    }

    ASSERT_TRUE(rig.controller.Start());
    const auto while_recording = rig.Import(good);
    rig.controller.Cancel();
    ASSERT_TRUE(std::holds_alternative<Error>(while_recording));
    EXPECT_EQ(std::get<Error>(while_recording).code, kSessionError);

    EXPECT_EQ(rig.reader.decodes.load(), 0) << "a refusal never waits on a decode";
    EXPECT_TRUE(HandleSessionList(rig.fixture.records)["sessions"].empty());
    EXPECT_TRUE(rig.Methods().empty());
}

TEST(Handlers, AReaderFailureAnswersInPlainWordsAndNeverWithTheFile) {
    ImportRig rig;
    const json params{{"path", "C:\\Home visit Jane Doe.m4a"},
                      {"startedAt", "2026-09-26T13:05:00Z"}};
    struct Row {
        const char* name;
        std::function<void()> fail;
        std::string reason;
    };
    const Row rows[] = {
        {"the reader's reason",
         [] { throw clinicavt::audio::RecordingError("this file is not a sound recording"); },
         "this file is not a sound recording"},
        {"anything else",
         [] {
             throw std::filesystem::filesystem_error(
                 "open", std::filesystem::path("C:\\Home visit Jane Doe.m4a"),
                 std::make_error_code(std::errc::permission_denied));
         },
         "the recording could not be read"},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        rig.reader.fail = row.fail;
        for (const auto& outcome :
             {HandleRecordingInspect(rig.reader, params), rig.Import(params)}) {
            ASSERT_TRUE(std::holds_alternative<Error>(outcome));
            EXPECT_EQ(std::get<Error>(outcome).code, kSessionError);
            EXPECT_EQ(std::get<Error>(outcome).data, json(row.reason));
        }
    }
    EXPECT_TRUE(HandleSessionList(rig.fixture.records)["sessions"].empty())
        << "no session began for a file that could not be opened";
    EXPECT_FALSE(rig.controller.Running());

    // A file that opens but fails to decode ends the import the same way, on its thread
    rig.reader.fail = nullptr;
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        rig.reader.fail_decode = row.fail;
        {
            const std::lock_guard<std::mutex> lock(rig.mutex);
            rig.pushed.clear();
        }
        ASSERT_TRUE(std::holds_alternative<json>(rig.Import(params)));
        const auto end = rig.WaitForEnd();
        ASSERT_TRUE(end.has_value());
        EXPECT_EQ(end->first, "session/importFailed");
        EXPECT_EQ(end->second["error"], json(row.reason));
        ASSERT_TRUE(WaitUntil([&] { return !rig.controller.Running(); }));
    }
    EXPECT_TRUE(HandleSessionList(rig.fixture.records)["sessions"].empty())
        << "the session each began is erased";
}

// Without --scripted, every way of starting a consultation is refused while a role is
// missing, before anything is stored or read
TEST(Handlers, AMissingModelRefusesStartImportAndSaysWhich) {
    ImportRig rig;
    const std::vector<std::string> missing{"asr", "diarisation", "segmentation"};
    const json fixture = LoadFixture("session-start-refused.json");
    const auto started =
        HandleSessionStart(rig.controller, nullptr, fixture["request"]["params"], missing, false);
    ASSERT_TRUE(std::holds_alternative<Error>(started));
    EXPECT_EQ(MakeError(std::int64_t{11}, std::get<Error>(started)), fixture["response"]);
    EXPECT_FALSE(rig.controller.Running());

    rig.reader.audio.assign(2 * 16000, 0.1F);
    const auto imported =
        rig.Import(json{{"path", "C:\\visit.m4a"}, {"startedAt", "2026-09-26T13:05:00Z"}}, missing);
    ASSERT_TRUE(std::holds_alternative<Error>(imported));
    EXPECT_EQ(std::get<Error>(imported).code, kSessionError);
    EXPECT_EQ(std::get<Error>(imported).data.value_or(json()), json(MissingModelsReason(missing)));
    EXPECT_EQ(rig.reader.decodes.load(), 0);
    EXPECT_TRUE(HandleSessionList(rig.fixture.records)["sessions"].empty());
    EXPECT_TRUE(rig.Methods().empty());
}

}  // namespace
}  // namespace clinicavt::ipc
