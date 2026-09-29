#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "adapters/ipc/handlers.hpp"
#include "handler_test_support.hpp"

namespace clinicavt::ipc {
namespace {

using namespace handler_test;

TEST(Handlers, SessionListTranscriptAndDeleteMatchTheFixtures) {
    using clinicavt::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->ReplaceTurns(
        id, std::vector<asr::Turn>{{480000, 48000, "", "about three weeks now, mostly mornings"}});
    fixture.store->Finalise(id);

    json list = HandleSessionList(fixture.records);
    ASSERT_EQ(list["sessions"].size(), 1u);
    EXPECT_EQ(list["sessions"][0]["id"], id);
    EXPECT_FALSE(list["sessions"][0]["endedAt"].get<std::string>().empty());
    EXPECT_EQ(list["sessions"][0]["label"], "") << "no note yet";
    EXPECT_TRUE(list["sessions"][0]["editedAt"].is_null());
    EXPECT_EQ(list["sessions"][0]["audioSeconds"], 33.0)
        << "the audio's length from the turns, which outlive the audio";
    // The fixture is the shape both languages agree on. It is named first because
    // iterating a temporary's sub-object dangles, as range-for extends only the top level
    const json list_fixture = LoadFixture("session-list.json");
    for (const auto& [key, value] : list_fixture["result"]["sessions"][0].items()) {
        EXPECT_TRUE(list["sessions"][0].contains(key)) << key;
    }

    const auto transcript = HandleSessionTranscript(*fixture.store, json{{"id", id}});
    ASSERT_TRUE(std::holds_alternative<json>(transcript));
    EXPECT_EQ(MakeResult(std::int64_t{8}, ResultOf(transcript)),
              LoadFixture("session-transcript.json"));

    fixture.store->SaveDocument(id, DocumentKind::kLabel, {.text = "Elbow swelling"});
    fixture.store->EditDocument(id, DocumentKind::kLabel, "Left elbow bursitis");
    list = HandleSessionList(fixture.records);
    EXPECT_EQ(list["sessions"][0]["label"], "Left elbow bursitis");
    EXPECT_TRUE(list["sessions"][0]["editedAt"].is_null())
        << "a retitle is housekeeping, not an edit to the record";
    fixture.store->SaveDocument(id, DocumentKind::kPatient, {.text = "sheet"});
    fixture.store->EditDocument(id, DocumentKind::kPatient, "sheet, edited");
    list = HandleSessionList(fixture.records);
    EXPECT_TRUE(list["sessions"][0]["editedAt"].is_string()) << "any document's edit counts";

    ASSERT_TRUE(
        std::holds_alternative<json>(HandleSessionDelete(*fixture.store, json{{"id", id}})));
    EXPECT_TRUE(HandleSessionList(fixture.records)["sessions"].empty());
    const auto missing = HandleSessionDelete(*fixture.store, json{{"id", "nope"}});
    ASSERT_TRUE(std::holds_alternative<Error>(missing));
    EXPECT_EQ(MakeError(std::int64_t{9}, std::get<Error>(missing)),
              LoadFixture("session-error.json"));

    for (const json params : {json::object(), json{{"id", 42}}}) {
        const auto no_transcript = HandleSessionTranscript(*fixture.store, params);
        ASSERT_TRUE(std::holds_alternative<Error>(no_transcript)) << params.dump();
        EXPECT_EQ(std::get<Error>(no_transcript).code, kInvalidParams) << params.dump();
        EXPECT_TRUE(std::holds_alternative<Error>(HandleSessionDelete(*fixture.store, params)))
            << params.dump();
    }
}

TEST(Handlers, SessionNoteAndPatientMatchTheFixtures) {
    using clinicavt::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto id = fixture.AddFinalisedSession();
    fixture.store->SaveDocument(id, DocumentKind::kNote,
                                {.text = "The patient presents with a swollen left elbow.",
                                 .style = "soap",
                                 .detail = "concise"});

    const auto outcome = HandleSessionNote(*fixture.store, json{{"id", id}});
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    const json note = ResultOf(outcome);
    EXPECT_EQ(note["text"], "The patient presents with a swollen left elbow.");
    EXPECT_EQ(note["style"], "soap");
    EXPECT_EQ(note["detail"], "concise");
    EXPECT_TRUE(note["generatedAt"].is_string());
    EXPECT_TRUE(note["editedAt"].is_null());
    const json note_fixture = LoadFixture("session-note.json");
    for (const auto& [key, value] : note_fixture["result"].items()) {
        EXPECT_TRUE(note.contains(key)) << key;
    }
    fixture.store->EditDocument(id, DocumentKind::kNote, "edited");
    const json edited = ResultOf(HandleSessionNote(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(edited["text"], "edited");
    EXPECT_TRUE(edited["editedAt"].is_string());
    EXPECT_TRUE(
        std::holds_alternative<Error>(HandleSessionNote(*fixture.store, json{{"id", "nope"}})));

    fixture.store->SaveDocument(id, DocumentKind::kPatient, {.text = "You have bursitis."});
    json patient = ResultOf(HandleSessionPatient(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(patient["text"], "You have bursitis.");
    EXPECT_TRUE(patient["generatedAt"].is_string());
    EXPECT_TRUE(patient["translation"].is_null());
    fixture.store->SaveDocument(id, DocumentKind::kTranslation,
                                {.text = "Masz zapalenie kaletki.", .language = "pl"});
    patient = ResultOf(HandleSessionPatient(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(patient["translation"]["language"], "pl");
    EXPECT_EQ(patient["translation"]["text"], "Masz zapalenie kaletki.");
    EXPECT_TRUE(patient["editedAt"].is_null());
    fixture.store->EditDocument(id, DocumentKind::kPatient, "You have bursitis of the elbow.");
    patient = ResultOf(HandleSessionPatient(*fixture.store, json{{"id", id}}));
    const auto translated_at = patient["translation"]["translatedAt"].get<std::string>();
    const auto edited_at = patient["editedAt"].get<std::string>();
    EXPECT_GE(edited_at, translated_at) << "the sheet changed after its translation";
    EXPECT_EQ(patient["translation"]["text"], "Masz zapalenie kaletki.") << "the edit keeps it";
    const json patient_fixture = LoadFixture("session-patient.json");
    for (const auto& [key, value] : patient_fixture["result"].items()) {
        EXPECT_TRUE(patient.contains(key)) << key;
    }
    for (const auto& [key, value] : patient_fixture["result"]["translation"].items()) {
        EXPECT_TRUE(patient["translation"].contains(key)) << key;
    }
}

// Replay reads any file the client names, so only an engine started with --allow-replay
// accepts it
TEST(Handlers, ReplayIsRefusedUnlessTheEngineAllowsIt) {
    ImportRig rig;
    const json replay{{"replay", {{"path", "C:\\any.wav"}, {"speed", 16.0}}}};
    const auto refused = HandleSessionStart(rig.controller, nullptr, replay, {}, false);
    ASSERT_TRUE(std::holds_alternative<Error>(refused));
    EXPECT_EQ(std::get<Error>(refused).code, kInvalidParams);
    EXPECT_FALSE(rig.controller.Running());

    const auto no_path =
        HandleSessionStart(rig.controller, nullptr, json{{"replay", json::object()}}, {}, true);
    ASSERT_TRUE(std::holds_alternative<Error>(no_path));
    EXPECT_EQ(std::get<Error>(no_path).code, kInvalidParams);

    const auto started = HandleSessionStart(rig.controller, nullptr, replay, {}, true);
    ASSERT_TRUE(std::holds_alternative<json>(started));
    EXPECT_TRUE(ResultOf(started)["sessionId"].is_string());
    EXPECT_TRUE(rig.controller.Running()) << "the stand-ins record, as with --scripted";
    rig.controller.Cancel();
}

TEST(Handlers, SessionNoteAndPatientMethodsMatchTheFixtures) {
    ServicesRig rig;
    WireShell shell;
    RegisterMethods(shell.server, rig.Services());
    ASSERT_NO_FATAL_FAILURE(shell.Connect());
    const auto call = [&](const char* name, const std::string& id = {}) {
        SCOPED_TRACE(name);
        const json fixture = LoadFixture(name);
        const json request = id.empty() ? fixture["request"] : ForSession(fixture["request"], id);
        EXPECT_EQ(shell.Call(request), fixture["response"]);
    };

    // The store picks the id, so the replies are compared with the fixture's id replaced
    const json start = LoadFixture("session-start.json");
    const json started = shell.Call(start["request"]);
    ASSERT_TRUE(started.contains("result")) << started.dump();
    const std::string recording = started["result"]["sessionId"].get<std::string>();
    json expected = start["response"];
    expected["result"]["sessionId"] = recording;
    EXPECT_EQ(started, expected);
    const json stop = LoadFixture("session-stop.json");
    expected = stop["response"];
    expected["result"]["sessionId"] = recording;
    EXPECT_EQ(shell.Call(stop["request"]), expected);
    ASSERT_TRUE(rig.WaitIdle());

    call("note-options.json");
    const auto id = rig.fixture.AddFinalisedSession();
    ASSERT_TRUE(rig.controller.Open(id));
    call("note-regenerate.json");
    ASSERT_TRUE(rig.WaitIdle());
    call("note-update.json", id);
    call("patient-update.json", id);
    call("patient-regenerate.json");
    ASSERT_TRUE(rig.WaitIdle());
    call("patient-translate.json", id);
    call("translate-languages.json");

    const auto other = rig.fixture.AddFinalisedSession();
    call("session-delete.json", other);
    call("session-cancel.json");
}

}  // namespace
}  // namespace clinicavt::ipc
