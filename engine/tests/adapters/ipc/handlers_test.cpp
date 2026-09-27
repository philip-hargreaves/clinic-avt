#include "adapters/ipc/handlers.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/guidance/guidance_lane.hpp"
#include "adapters/ipc/pipe_client.hpp"
#include "adapters/ipc/wire_events.hpp"
#include "adapters/storage/sqlite_session_store.hpp"
#include "core/common/version.hpp"
#include "ports/store_error.hpp"

namespace clinicavt::ipc {
namespace {

const json& ResultOf(const std::variant<json, Error>& outcome) {
    return std::get<json>(outcome);
}

json LoadFixture(const std::string& name) {
    std::ifstream in(std::string(CLINICAVT_FIXTURE_DIR) + "/" + name);
    if (!in.is_open()) throw std::runtime_error("missing fixture: " + name);
    return json::parse(in);
}

// A fresh directory under temp, gone again when the test ends
struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const std::string& name)
        : path(std::filesystem::temp_directory_path() / ("clinicavt-handlers-" + name)) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void StageModel(const std::filesystem::path& root, const std::string& id, const std::string& task,
                const std::string& tier, const std::string& extra = "") {
    std::filesystem::create_directories(root / id);
    std::ofstream(root / id / "manifest.json")
        << R"({"manifestVersion": 1, "id": ")" << id << R"(",)" << extra << R"( "task": ")" << task
        << R"(", "tier": ")" << tier << R"(", "licence": "MIT",)"
        << R"( "runtime": {"device": "GPU"}, "files": {"model.xml": "00"}})";
}

TEST(Handlers, HelloAndEchoRefuseWhatTheyCannotParse) {
    // Each of these fails PeerInfoFromJson for a different reason
    const json hellos[] = {
        json::object(),
        json{{"name", "shell"}},
        json{{"name", 1}, {"version", "0.1.0"}, {"protocolVersion", kProtocolVersion}},
        json{{"name", "shell"}, {"version", "0.1.0"}, {"protocolVersion", kProtocolVersion + 1}},
        // Truncated to 32 bits this would read as 0
        json{{"name", "shell"},
             {"version", "0.1.0"},
             {"protocolVersion", static_cast<std::uint64_t>(1) << 33}},
        json{{"name", "shell"},
             {"version", "0.1.0"},
             {"protocolVersion", kProtocolVersion},
             {"extra", 1}},
    };
    for (const auto& params : hellos) {
        const auto outcome = HandleHello(params);
        ASSERT_TRUE(std::holds_alternative<Error>(outcome)) << params.dump();
        EXPECT_EQ(std::get<Error>(outcome).code, kInvalidParams) << params.dump();
    }

    const json echoes[] = {
        json::object(),
        json{{"payload", 42}},
        json{{"payload", nullptr}},
        json{{"payload", json::array({"a"})}},
    };
    for (const auto& params : echoes) {
        const auto outcome = HandleEcho(params);
        ASSERT_TRUE(std::holds_alternative<Error>(outcome)) << params.dump();
        EXPECT_EQ(std::get<Error>(outcome).code, kInvalidParams) << params.dump();
    }

    // Whatever the peer claims about itself, the engine answers as itself
    const auto outcome = HandleHello(
        json{{"name", "impostor"}, {"version", "9.9.9"}, {"protocolVersion", kProtocolVersion}});
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    EXPECT_EQ(ResultOf(outcome)["name"], clinicavt::kName);
    EXPECT_EQ(ResultOf(outcome)["version"], clinicavt::kVersion);
}

TEST(Handlers, ModelsListMatchesTheFixtureAndMarksTheConfiguredTier) {
    const TempDir asr("models");
    StageModel(asr.path, "whisper-turbo-int8", "asr", "default",
               R"( "name": "Whisper Large v3 Turbo",)");
    const json built =
        MakeResult(std::int64_t{7}, HandleModels(clinicavt::models::ModelStore(asr.path)));
    EXPECT_EQ(built, LoadFixture("models-list.json"));

    // With two note models staged, the configured tier is the active one whatever
    // order the store lists them in
    const TempDir notes("tiers");
    StageModel(notes.path, "qwen3.5-9b-int4", "note", "default");
    StageModel(notes.path, "qwen3.6-35b-a3b-int4", "note", "accuracy");
    const json listed = HandleModels(clinicavt::models::ModelStore(notes.path), "accuracy");
    ASSERT_EQ(listed["models"].size(), 2u);
    for (const auto& model : listed["models"]) {
        EXPECT_EQ(model["active"], model["tier"] == "accuracy") << model.dump();
    }
}

// A lane that records what it was asked and answers with a state
struct FakeLane : clinicavt::note::INoteLane {
    std::vector<std::string> configured;
    clinicavt::note::NoteModelState state;
    std::string refuse;  // Configure throws this as invalid_argument when set

    clinicavt::note::NoteModelState Configure(const std::string& tier) override {
        if (!refuse.empty()) throw std::invalid_argument(refuse);
        configured.push_back(tier);
        state.tier = tier;
        state.phase = clinicavt::note::NoteModelState::Phase::kLoading;
        return state;
    }

    clinicavt::note::NoteModelState State() const override {
        return state;
    }

    void SetListener(Listener) override {}
};

TEST(Handlers, NoteTierLoadsATierAndRefusesWhatItCannotServe) {
    FakeLane lane;
    lane.state.id = "qwen3.6-35b-a3b-int4";
    lane.state.name = "Qwen3.6 35B";

    auto outcome = HandleNoteTier(&lane, false, json{{"tier", "accuracy"}});
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    EXPECT_EQ(lane.configured, std::vector<std::string>{"accuracy"});
    EXPECT_EQ(ResultOf(outcome), LoadFixture("note-tier.json")["response"]["result"]);

    // Something that is not a tier never reaches the lane
    lane.configured.clear();
    outcome = HandleNoteTier(&lane, false, json{{"tier", "premium"}});
    ASSERT_TRUE(std::holds_alternative<Error>(outcome));
    EXPECT_EQ(std::get<Error>(outcome).code, kInvalidParams);
    EXPECT_TRUE(lane.configured.empty());

    // A tier with nothing staged returns the store's message as a parameter error
    lane.refuse = "no model for note/accuracy; installed: qwen3.5-9b-int4(note/default)";
    outcome = HandleNoteTier(&lane, false, json{{"tier", "accuracy"}});
    ASSERT_TRUE(std::holds_alternative<Error>(outcome));
    EXPECT_EQ(std::get<Error>(outcome).code, kInvalidParams);
    EXPECT_NE(std::get<Error>(outcome).data->dump().find("qwen3.5-9b-int4"), std::string::npos);

    // No note lane at all, when nothing is staged or no host sits beside the engine
    outcome = HandleNoteTier(nullptr, false, json{{"tier", "default"}});
    ASSERT_TRUE(std::holds_alternative<Error>(outcome));
    EXPECT_EQ(std::get<Error>(outcome).code, kSessionError);

    // The note/model notification the lane's listener sends
    clinicavt::note::NoteModelState ready;
    ready.phase = clinicavt::note::NoteModelState::Phase::kReady;
    ready.tier = "accuracy";
    ready.id = "qwen3.6-35b-a3b-int4";
    ready.name = "Qwen3.6 35B";
    ready.seconds = 27.4;
    const json fixture = LoadFixture("note-model.json");
    EXPECT_EQ(NoteModelJson(ready), fixture["params"]);
    EXPECT_EQ(fixture["method"], "note/model");
}

// Low-power mode moves speech recognition in place, so the note model stays loaded
TEST(Handlers, AsrDeviceMovesSpeechRecognitionAndSaysWhenItIsReady) {
    std::string moved_to;
    std::function<void(const std::string&)> finish;
    const AsrSwitch switcher = [&](const std::string& device, auto done) {
        moved_to = device;
        finish = std::move(done);
        return true;
    };
    std::vector<json> notices;
    const auto notify = [&notices](json state) { notices.push_back(std::move(state)); };

    const auto outcome = HandleAsrDevice(switcher, false, json{{"device", "NPU"}}, notify);

    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    EXPECT_EQ(ResultOf(outcome)["state"], "loading");
    EXPECT_EQ(moved_to, "NPU");
    finish("");
    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0], (json{{"device", "NPU"}, {"state", "ready"}}));
    finish("no NPU");
    EXPECT_EQ(notices[1]["state"], "failed");
    EXPECT_EQ(notices[1]["detail"], "no NPU");

    // Never an unknown device, never without a model to move
    moved_to.clear();
    EXPECT_EQ(
        std::get<Error>(HandleAsrDevice(switcher, false, json{{"device", "CPU"}}, notify)).code,
        kInvalidParams);
    EXPECT_TRUE(moved_to.empty());
    EXPECT_EQ(std::get<Error>(HandleAsrDevice({}, false, json{{"device", "GPU"}}, notify)).code,
              kSessionError);
}

struct SessionStoreFixture {
    std::filesystem::path root;
    std::unique_ptr<clinicavt::store::SqliteSessionStore> store;

    SessionStoreFixture() {
        root = std::filesystem::temp_directory_path() /
               ("clinicavt-handlers-sessions-" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        store = std::make_unique<clinicavt::store::SqliteSessionStore>(root, std::chrono::hours(1));
    }

    ~SessionStoreFixture() {
        store.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::string AddFinalisedSession() const {
        const auto id = store->Begin({16000, "", ""});
        store->Finalise(id);
        return id;
    }
};

// A running consultation keeps its note model, its speech model, the clinician's
// voiceprint and every stored record
TEST(Handlers, WhatWouldDisturbARunningConsultationIsRefused) {
    FakeLane lane;
    std::string moved_to;
    const AsrSwitch switcher = [&](const std::string& device, auto) {
        moved_to = device;
        return true;
    };
    const TempDir anchor_root("anchor-running");
    clinicavt::diar::AnchorStore anchors(anchor_root.path);
    anchors.Accrue(std::vector<float>{1.0f, 0.0f});
    SessionStoreFixture fixture;
    fixture.AddFinalisedSession();

    struct Case {
        const char* method;
        std::function<std::variant<json, Error>()> call;
    };
    const Case cases[] = {
        {"note/tier", [&] { return HandleNoteTier(&lane, true, json{{"tier", "accuracy"}}); }},
        {"asr/device",
         [&] { return HandleAsrDevice(switcher, true, json{{"device", "GPU"}}, [](json) {}); }},
        {"anchor/clear", [&] { return HandleAnchorClear(anchors, true); }},
        {"session/deleteAll", [&] { return HandleSessionDeleteAll(*fixture.store, true); }},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.method);
        const auto outcome = c.call();
        ASSERT_TRUE(std::holds_alternative<Error>(outcome));
        EXPECT_EQ(std::get<Error>(outcome).code, kSessionError);
    }

    EXPECT_TRUE(lane.configured.empty());
    EXPECT_TRUE(moved_to.empty());
    EXPECT_TRUE(anchors.Anchor().has_value());
    EXPECT_EQ(HandleSessionList(*fixture.store)["sessions"].size(), 1u);
}

TEST(Handlers, SessionListTranscriptAndDeleteMatchTheFixtures) {
    using clinicavt::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->ReplaceTurns(
        id, std::vector<asr::Turn>{{480000, 48000, "", "about three weeks now, mostly mornings"}});
    fixture.store->Finalise(id);

    json list = HandleSessionList(*fixture.store);
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
    list = HandleSessionList(*fixture.store);
    EXPECT_EQ(list["sessions"][0]["label"], "Left elbow bursitis");
    EXPECT_TRUE(list["sessions"][0]["editedAt"].is_null())
        << "a retitle is housekeeping, not an edit to the record";
    fixture.store->SaveDocument(id, DocumentKind::kPatient, {.text = "sheet"});
    fixture.store->EditDocument(id, DocumentKind::kPatient, "sheet, edited");
    list = HandleSessionList(*fixture.store);
    EXPECT_TRUE(list["sessions"][0]["editedAt"].is_string()) << "any document's edit counts";

    ASSERT_TRUE(
        std::holds_alternative<json>(HandleSessionDelete(*fixture.store, json{{"id", id}})));
    EXPECT_TRUE(HandleSessionList(*fixture.store)["sessions"].empty());
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
    const json patient_fixture = LoadFixture("session-patient.json");
    for (const auto& [key, value] : patient_fixture["result"].items()) {
        EXPECT_TRUE(patient.contains(key)) << key;
    }
}

// Seeds once, lists as samples, clears without touching the real session
TEST(Handlers, TheSampleYearSeedsOnceAndClearsCleanly) {
    using clinicavt::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto real = fixture.AddFinalisedSession();

    const auto seeded = HandleDemoSeed(*fixture.store, CLINICAVT_DEMO_DIR);
    ASSERT_TRUE(std::holds_alternative<json>(seeded))
        << (std::holds_alternative<Error>(seeded) ? std::get<Error>(seeded).message : "");
    EXPECT_EQ(MakeResult(std::int64_t{30}, ResultOf(seeded)), LoadFixture("demo-seed.json"));
    const auto again = HandleDemoSeed(*fixture.store, CLINICAVT_DEMO_DIR);
    ASSERT_TRUE(std::holds_alternative<json>(again));
    EXPECT_EQ(ResultOf(again)["added"], 0) << "already seeded is a no-op, not an error";

    const json sessions = HandleSessionList(*fixture.store)["sessions"];
    EXPECT_EQ(sessions.size(), 9u);
    std::size_t samples = 0;
    for (const auto& s : sessions) {
        if (s["demo"].get<bool>()) {
            samples += 1;
            EXPECT_FALSE(s["label"].get<std::string>().empty());
            EXPECT_GT(s["audioSeconds"].get<double>(), 300.0);
        } else {
            EXPECT_EQ(s["id"], real);
        }
    }
    EXPECT_EQ(samples, 8u);
    const json entries = HandleReflectionList(*fixture.store)["reflections"];
    ASSERT_EQ(entries.size(), 8u);
    for (const auto& e : entries) EXPECT_TRUE(e["demo"].get<bool>());
    const auto sample = entries[0]["id"].get<std::string>();
    EXPECT_FALSE(fixture.store->ReadDocument(sample, DocumentKind::kNote).text.empty());
    EXPECT_FALSE(fixture.store->ReadDocument(sample, DocumentKind::kPatient).text.empty());

    EXPECT_EQ(MakeResult(std::int64_t{31}, HandleDemoClear(*fixture.store)),
              LoadFixture("demo-clear.json"));
    const json left = HandleSessionList(*fixture.store)["sessions"];
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0]["id"], real);
    EXPECT_EQ(HandleReflectionList(*fixture.store)["reflections"].size(), 0u);

    // One erase removes samples and real sessions alike
    ASSERT_TRUE(std::holds_alternative<json>(HandleDemoSeed(*fixture.store, CLINICAVT_DEMO_DIR)));
    const auto erased = HandleSessionDeleteAll(*fixture.store, false);
    ASSERT_TRUE(std::holds_alternative<json>(erased));
    EXPECT_EQ(ResultOf(erased)["removed"], 9);
    EXPECT_EQ(HandleSessionList(*fixture.store)["sessions"].size(), 0u);
}

TEST(Handlers, AStoredSummaryLeavesScrubbedWhateverWasStored) {
    using clinicavt::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto id = fixture.AddFinalisedSession();
    fixture.store->SaveDocument(id, DocumentKind::kSummary,
                                {.text = "A 53-year-old male presented with a swollen elbow."});

    const json got = ResultOf(HandleReflectionGet(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(got["summary"]["text"], "A male in their fifties presented with a swollen elbow.");
    const json listed = HandleReflectionList(*fixture.store)["reflections"];
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0]["summary"], "A male in their fifties presented with a swollen elbow.");

    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        *fixture.store, json{{"id", id}, {"summary", "The patient, aged 67, was seen."}})));
    EXPECT_EQ(fixture.store->ReadDocument(id, DocumentKind::kSummary).text,
              "The patient, in their sixties, was seen.");
}

TEST(Handlers, ReflectionGetUpdateListAndDelete) {
    using clinicavt::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto id = fixture.AddFinalisedSession();
    fixture.store->SaveDocument(id, DocumentKind::kLabel, {.text = "Elbow swelling"});

    // Before anything is written there is only a label, both parts are null and it is unlisted
    json got = ResultOf(HandleReflectionGet(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(got["label"], "Elbow swelling");
    EXPECT_TRUE(got["summary"].is_null());
    EXPECT_TRUE(got["reflection"].is_null());
    EXPECT_EQ(HandleReflectionList(*fixture.store)["reflections"].size(), 0u);

    // A summary alone is an entry because the sheet was opened and the writing can follow
    fixture.store->SaveDocument(id, DocumentKind::kSummary,
                                {.text = "A patient in their forties."});
    json listed = HandleReflectionList(*fixture.store)["reflections"];
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0]["learned"], "");
    EXPECT_EQ(listed[0]["summary"], "A patient in their forties.");
    EXPECT_TRUE(listed[0]["createdAt"].is_string());

    // Three empty answers keep the entry. Only delete removes it
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        *fixture.store, json{{"id", id}, {"happened", ""}, {"learned", ""}, {"next", ""}})));
    EXPECT_EQ(HandleReflectionList(*fixture.store)["reflections"].size(), 1u);

    // A first answer creates the entry and a later one merges into it
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        *fixture.store, json{{"id", id}, {"learned", "check the temperature"}})));
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        *fixture.store, json{{"id", id}, {"next", "add a red-flag check"}})));
    got = ResultOf(HandleReflectionGet(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(got["reflection"]["happened"], "");
    EXPECT_EQ(got["reflection"]["learned"], "check the temperature");
    EXPECT_EQ(got["reflection"]["next"], "add a red-flag check");
    EXPECT_TRUE(got["reflection"]["createdAt"].is_string());
    EXPECT_TRUE(got["reflection"]["editedAt"].is_string()) << "the merge is an edit";
    const json fixture_shape = LoadFixture("reflection-get.json")["result"];
    for (const auto& [key, value] : fixture_shape.items()) {
        EXPECT_TRUE(got.contains(key)) << key;
    }
    for (const auto& [key, value] : fixture_shape["reflection"].items()) {
        EXPECT_TRUE(got["reflection"].contains(key)) << key;
    }

    // Ticked references keep their own words and replace as a set. Answers stay
    EXPECT_TRUE(got["reflection"]["references"].is_array());
    EXPECT_TRUE(got["reflection"]["references"].empty());
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        *fixture.store,
        json{{"id", id},
             {"references", json::array({{{"key", "nice:ng100"},
                                          {"reference", "NG100"},
                                          {"title", "Rheumatoid arthritis in adults"},
                                          {"link", "https://www.nice.org.uk/guidance/ng100"},
                                          {"source", "NICE"}},
                                         {{"key", "upload:doc-7"}, {"title", "Leaflet"}}})}})));
    got = ResultOf(HandleReflectionGet(*fixture.store, json{{"id", id}}));
    ASSERT_EQ(got["reflection"]["references"].size(), 2u);
    EXPECT_EQ(got["reflection"]["references"][0]["reference"], "NG100");
    EXPECT_EQ(got["reflection"]["references"][1]["link"], "") << "missing fields read empty";
    EXPECT_EQ(got["reflection"]["learned"], "check the temperature");
    ASSERT_TRUE(std::holds_alternative<json>(
        HandleReflectionUpdate(*fixture.store, json{{"id", id}, {"references", json::array()}})));
    got = ResultOf(HandleReflectionGet(*fixture.store, json{{"id", id}}));
    EXPECT_TRUE(got["reflection"]["references"].empty());
    EXPECT_TRUE(std::holds_alternative<Error>(HandleReflectionUpdate(
        *fixture.store, json{{"id", id}, {"references", json::array({"NG100"})}})));

    // The summary rides on the same update when the clinician corrects it
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        *fixture.store, json{{"id", id}, {"summary", "A patient in their forties."}})));
    got = ResultOf(HandleReflectionGet(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(got["summary"]["text"], "A patient in their forties.");
    EXPECT_TRUE(got["summary"]["editedAt"].is_string());

    // Listed with the card's line
    const json list = HandleReflectionList(*fixture.store)["reflections"];
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0]["id"], id);
    EXPECT_EQ(list[0]["label"], "Elbow swelling");
    EXPECT_EQ(list[0]["learned"], "check the temperature");
    // Named first because iterating a temporary's sub-object dangles
    const json list_fixture = LoadFixture("reflection-list.json");
    for (const auto& [key, value] : list_fixture["result"]["reflections"][0].items()) {
        EXPECT_TRUE(list[0].contains(key)) << key;
    }

    // Wrong types are parameter errors, unknown ids session errors
    EXPECT_TRUE(std::holds_alternative<Error>(
        HandleReflectionUpdate(*fixture.store, json{{"id", id}, {"learned", 3}})));
    EXPECT_TRUE(
        std::holds_alternative<Error>(HandleReflectionGet(*fixture.store, json{{"id", "nope"}})));

    ASSERT_TRUE(
        std::holds_alternative<json>(HandleReflectionDelete(*fixture.store, json{{"id", id}})));
    got = ResultOf(HandleReflectionGet(*fixture.store, json{{"id", id}}));
    EXPECT_TRUE(got["reflection"].is_null());
    EXPECT_TRUE(got["summary"].is_null());
    EXPECT_EQ(HandleReflectionList(*fixture.store)["reflections"].size(), 0u);
}

TEST(Handlers, AudioInputsCarryThePickerFields) {
    const std::vector<clinicavt::audio::CaptureDevice> devices{
        {"{0.0.1}.{aa}", "Microphone Array (Realtek(R) Audio)", "Microphone Array", true, false},
        {"{0.0.1}.{bb}", "Headset (H800 Hands-Free)", "Headset", false, true},
    };

    const json result = HandleAudioInputs(devices);

    ASSERT_EQ(result["devices"].size(), 2u);
    EXPECT_EQ(result["devices"][0]["id"], "{0.0.1}.{aa}");
    EXPECT_EQ(result["devices"][0]["name"], "Microphone Array (Realtek(R) Audio)");
    EXPECT_EQ(result["devices"][0]["shortName"], "Microphone Array");
    EXPECT_EQ(result["devices"][0]["isDefault"], true);
    EXPECT_EQ(result["devices"][0]["bluetooth"], false);
    EXPECT_EQ(result["devices"][1]["bluetooth"], true);

    // No microphones is an empty list, not an error
    const json none = HandleAudioInputs({});
    EXPECT_TRUE(none["devices"].is_array());
    EXPECT_TRUE(none["devices"].empty());
}

TEST(Handlers, AnchorStatusMatchesTheFixtureAndClearForgets) {
    const TempDir root("anchor");
    clinicavt::diar::AnchorStore anchors(root.path);
    EXPECT_EQ(MakeResult(std::int64_t{3}, HandleAnchorStatus(anchors)),
              LoadFixture("anchor-status.json"));

    anchors.Accrue(std::vector<float>{1.0f, 0.0f});
    auto status = HandleAnchorStatus(anchors);
    EXPECT_EQ(status["origin"], "accrued");
    EXPECT_EQ(status["sessions"], 1);

    anchors.Replace(std::vector<float>{0.0f, 1.0f}, 1'757'000'000);
    status = HandleAnchorStatus(anchors);
    EXPECT_EQ(status["origin"], "enrolled");
    EXPECT_EQ(status["sessions"], 0);
    EXPECT_EQ(status["enrolledAt"], 1'757'000'000);

    ASSERT_TRUE(std::holds_alternative<json>(HandleAnchorClear(anchors, false)));
    EXPECT_FALSE(anchors.Anchor().has_value());
    EXPECT_EQ(HandleAnchorStatus(anchors)["origin"], "none");
}

// Answers every search with one result that names the note it was given
struct EchoRetriever : clinicavt::guidance::IGuidanceRetriever {
    std::mutex mutex;
    std::vector<std::pair<std::string, int>> searches;
    std::vector<clinicavt::guidance::SearchMode> modes;

    clinicavt::guidance::Results Search(const std::string& note, int limit,
                                        clinicavt::guidance::SearchMode mode) override {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            searches.emplace_back(note, limit);
            modes.push_back(mode);
        }
        clinicavt::guidance::Results results;
        results.considered = 1;
        clinicavt::guidance::Result one;
        one.chunk_id = "fx100-1_1_1";
        one.trigger = note;
        results.shown.push_back(one);
        return results;
    }
    std::vector<clinicavt::guidance::Corpus> Corpora() override {
        return {};
    }
    clinicavt::guidance::Readiness Status() override {
        return {clinicavt::guidance::Readiness::Phase::kReady, ""};
    }
};

// Notifications the lane sent, waitable
struct Sent {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<std::string, json>> all;

    Notify Sink() {
        return [this](const std::string& method, json params) {
            const std::lock_guard<std::mutex> lock(mutex);
            all.emplace_back(method, std::move(params));
            changed.notify_all();
        };
    }
    bool WaitFor(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&] { return all.size() >= count; });
    }
};

TEST(Handlers, GuidanceReadyMatchesTheFixture) {
    clinicavt::guidance::Results results;
    results.considered = 40;
    results.floor = 0.85;
    results.upload_floor = 0.85;
    clinicavt::guidance::Corpus structured;
    structured.id = "fixture-nice";
    structured.name = "Fixture guidance corpus (structured)";
    structured.licence = "invented";
    structured.attribution = "none";
    structured.label = "NICE";
    structured.source = "nice";
    structured.embedder = "gte-large-int8";
    structured.sha256 = "e4f1be59be8647759ccd16d916ba9504b464f39a2799cb598ca5f0e4dc779a9f";
    structured.chunks = 40;
    structured.built_at = "2026-09-11T00:00:00Z";
    results.searched.push_back(structured);
    clinicavt::guidance::Corpus plain;
    plain.id = "fixture";
    plain.name = "Fixture guidance corpus";
    plain.licence = "invented";
    plain.attribution = "none";
    plain.source = "text";
    plain.embedder = "gte-large-int8";
    plain.sha256 = "1b7d2e9f0a3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6";
    plain.chunks = 5;
    plain.built_at = "2026-09-12T00:00:00Z";
    results.searched.push_back(plain);
    clinicavt::guidance::Result one;
    one.corpus = "fixture-nice";
    one.chunk_id = "fx100-1_1_1";
    one.code = "fx100";
    one.number = "1.1.1";
    one.title = "Fictional inflammatory joint disease: assessment and management";
    one.section = "1.1 Referral, diagnosis and investigations > Referral from primary care";
    one.text =
        "Refer adults with persistent synovitis of undetermined cause to a specialist, and refer "
        "urgently if the small joints of the hands or feet are affected.";
    one.url = "https://example.test/guidance/fx100/chapter/1-recommendations#fx100-1_1_1";
    one.last_updated = "2020-10-12";
    one.update_tag = "2009, amended 2018";
    one.source = "nice";
    one.citation = "FX100 1.1.1, Fictional inflammatory joint disease: assessment and management";
    one.score = 0.8971234;
    one.trigger = "Examination shows synovitis of several MCP joints.";
    results.shown.push_back(one);
    clinicavt::guidance::Result two;  // plain text with no section, date or tag, a whole-note hit
    two.corpus = "fixture";
    two.chunk_id = "gout-1";
    two.code = "gout";
    two.number = "1.1.1";
    two.title = "Fictional gout guideline";
    two.text = "1.1.1 Offer an NSAID or colchicine for an acute flare.";
    two.url = "Gout.md";
    two.source = "text";
    two.citation = "GOUT 1.1.1, Fictional gout guideline";
    two.score = 0.861;
    results.shown.push_back(two);

    SessionStoreFixture fixture;
    const auto id = fixture.AddFinalisedSession();
    fixture.store->SaveDocument(id, clinicavt::store::DocumentKind::kNote, {.text = "note"});
    const auto note = fixture.store->ReadDocument(id, clinicavt::store::DocumentKind::kNote);
    Sent sent;
    std::string stored_first;
    auto request = GuidanceSearchRequest(
        *fixture.store, id, note, 3, [&](const std::string& method, json params) {
            stored_first =
                fixture.store->ReadDocument(id, clinicavt::store::DocumentKind::kGuidance).text;
            sent.Sink()(method, std::move(params));
        });
    request.on_ready(results);
    json expected = LoadFixture("guidance-ready.json");
    expected["params"]["id"] = id;
    ASSERT_EQ(sent.all.size(), 1u);
    EXPECT_EQ(expected["method"], sent.all[0].first);
    EXPECT_EQ(sent.all[0].second, expected["params"]);

    // Stored before it was sent, as the same record
    json record = sent.all[0].second;
    record.erase("id");
    record.erase("storeError");
    record.erase("stale");
    EXPECT_EQ(json::parse(stored_first), record);
}

TEST(Handlers, GuidanceReadyIsStaleDroppedOrCarriesTheStoreError) {
    using clinicavt::store::DocumentKind;
    SessionStoreFixture fixture;

    // The note moved while the search ran: the results arrive marked stale
    const auto moved = fixture.AddFinalisedSession();
    fixture.store->SaveDocument(moved, DocumentKind::kNote, {.text = "note"});
    Sent stale;
    auto request = GuidanceSearchRequest(*fixture.store, moved,
                                         fixture.store->ReadDocument(moved, DocumentKind::kNote), 3,
                                         stale.Sink());
    fixture.store->EditDocument(moved, DocumentKind::kNote, "edited meanwhile");
    request.on_ready(clinicavt::guidance::Results{});
    ASSERT_EQ(stale.all.size(), 1u);
    EXPECT_TRUE(stale.all[0].second["stale"]);

    // The session was erased meanwhile: nothing is sent and nothing is stored
    const auto erased = fixture.AddFinalisedSession();
    fixture.store->SaveDocument(erased, DocumentKind::kNote, {.text = "note"});
    Sent dropped;
    request = GuidanceSearchRequest(*fixture.store, erased,
                                    fixture.store->ReadDocument(erased, DocumentKind::kNote), 3,
                                    dropped.Sink());
    fixture.store->Delete(erased);
    request.on_ready(clinicavt::guidance::Results{});
    EXPECT_TRUE(dropped.all.empty());

    // The store refuses a session still recording: the results still arrive, with the reason
    const auto recording = fixture.store->Begin({16000, "", ""});
    Sent refused;
    request = GuidanceSearchRequest(*fixture.store, recording, {.text = "note"}, 3, refused.Sink());
    request.on_ready(clinicavt::guidance::Results{});
    ASSERT_EQ(refused.all.size(), 1u);
    EXPECT_EQ(refused.all[0].first, "guidance/ready");
    EXPECT_EQ(refused.all[0].second["id"], recording);
    EXPECT_NE(refused.all[0].second["storeError"].get<std::string>().find("still recording"),
              std::string::npos);
}

TEST(Handlers, GuidanceStateMatchesTheFixtures) {
    SessionStoreFixture sessions;
    Sent sent;
    auto request = GuidanceSearchRequest(*sessions.store, "a1b2c3d4e5f60718293a4b5c6d7e8f90",
                                         {.text = "note"}, 3, sent.Sink());
    request.on_failed("guidance embedder gte-large-int8: tokenizer ignores max_length");
    const json failed = LoadFixture("guidance-failed.json");
    ASSERT_EQ(sent.all.size(), 1u);
    EXPECT_EQ(failed["method"], sent.all[0].first);
    EXPECT_EQ(sent.all[0].second, failed["params"]);

    clinicavt::guidance::Corpus loaded;
    loaded.id = "fixture";
    loaded.name = "Fixture guidance corpus";
    loaded.licence = "invented";
    loaded.attribution = "none";
    loaded.source = "text";
    loaded.embedder = "gte-large-int8";
    loaded.sha256 = "e4f1be59be8647759ccd16d916ba9504b464f39a2799cb598ca5f0e4dc779a9f";
    loaded.chunks = 40;
    loaded.built_at = "2026-09-11T00:00:00Z";
    clinicavt::guidance::Corpus refused;
    refused.id = "nice-2026-08-25";
    refused.unavailable = "corpus.db sha256 does not match the manifest";
    clinicavt::guidance::Readiness ready;
    ready.phase = clinicavt::guidance::Readiness::Phase::kReady;
    EXPECT_EQ(GuidanceCorporaJson(ready, {loaded, refused}),
              LoadFixture("guidance-corpora.json")["result"]);

    clinicavt::guidance::Readiness unavailable;
    unavailable.phase = clinicavt::guidance::Readiness::Phase::kUnavailable;
    unavailable.detail = "no model for embedding/default";
    const json model = LoadFixture("guidance-model.json");
    EXPECT_EQ(model["method"], "guidance/model");
    EXPECT_EQ(GuidanceModelJson(unavailable), model["params"]);
    EXPECT_EQ(GuidanceModelJson(clinicavt::guidance::Readiness{})["state"], "loading");
}

clinicavt::guidance::DocumentInfo ReadyDocument() {
    clinicavt::guidance::DocumentInfo d;
    d.path = "BSR gout guideline 2017.md";
    d.sha256 = "bc9860cdfed1879752e67ea846b7bbef6f59290b9f16988e7ff977769ac60b3c";
    d.id = 7302914125883421;
    d.name = "BSR gout guideline 2017";
    d.mime = "text/markdown";
    d.state = "ready";
    d.added_at = "2026-09-15T09:12:44Z";
    d.indexed_at = "2026-09-15T09:13:02Z";
    d.bytes = 48213;
    d.chunks = 41;
    return d;
}

clinicavt::guidance::DocumentInfo IndexingDocument() {
    clinicavt::guidance::DocumentInfo d;
    d.path = "PMR local pathway.txt";
    d.sha256 = "7e3f9e26abe9d7567618d8c8cf96083afd0a7e4bfe3a3b553aad3b2c26cbf2f6";
    d.id = 2871034561297730;
    d.name = "PMR local pathway";
    d.mime = "text/plain";
    d.state = "indexing";
    d.added_at = "2026-09-15T09:14:10Z";
    d.bytes = 9120;
    return d;
}

clinicavt::guidance::DocumentInfo FailedDocument() {
    clinicavt::guidance::DocumentInfo d;
    d.path = "Clinic letter.txt";
    d.sha256 = "5135b53eff5763333bbc3e0e03acd94f020be0b3396d05f326f3e1388dc23a0f";
    d.id = 9106572248130415;
    d.name = "Clinic letter";
    d.mime = "text/plain";
    d.state = "failed";
    d.error = "patientData";
    d.added_at = "2026-09-15T09:14:10Z";
    d.bytes = 2210;
    return d;
}

struct FakeIngest : clinicavt::guidance::IDocumentIngest {
    std::vector<std::filesystem::path> added;
    std::vector<std::int64_t> removed;
    std::vector<std::pair<int, std::int64_t>> rendered;

    clinicavt::guidance::Accepted Add(const std::vector<std::filesystem::path>& paths) override {
        added = paths;
        clinicavt::guidance::Accepted out;
        out.documents.push_back(IndexingDocument());
        out.skipped.push_back({"C:\\Guidelines\\scan.pdf", "unsupported"});
        out.skipped.push_back({"C:\\Guidelines\\empty.txt", "unreadable"});
        return out;
    }
    clinicavt::guidance::Listing List() override {
        return {Folder(), true, 2, {ReadyDocument(), IndexingDocument(), FailedDocument()}};
    }
    void Remove(std::int64_t id) override {
        Known(id);
        removed.push_back(id);
    }
    std::size_t RemoveAll() override {
        return 3;
    }
    clinicavt::guidance::PageRender Render(std::int64_t id, int page, std::int64_t chunk) override {
        Known(id);
        rendered.emplace_back(page, chunk);
        return {Scratch() / "page-7302914125883421-2.bmp", 1191, 1684, 5,
                R"([{"page":2,"left":0.118,"top":0.412,"right":0.882,"bottom":0.463},)"
                R"({"page":3,"left":0.118,"top":0.094,"right":0.882,"bottom":0.121}])"};
    }
    std::filesystem::path Path(std::int64_t id) override {
        Known(id);
        return Folder() / "BSR gout guideline 2017.md";
    }
    static void Known(std::int64_t id) {
        if (id != ReadyDocument().id) {
            throw clinicavt::store::StoreError(clinicavt::store::StoreCode::kNotFound,
                                               "no document");
        }
    }
    static std::filesystem::path Folder() {
        return R"(C:\Users\clinician\Documents\ClinicAVT guidelines)";
    }
    static std::filesystem::path Scratch() {
        return R"(C:\Users\clinician\AppData\Local\clinicavt\store\documents\scratch)";
    }
    void SetListener(std::function<void(const clinicavt::guidance::IngestProgress&)>,
                     std::function<void(const clinicavt::guidance::DocumentInfo&)>) override {}
};

TEST(Handlers, DocumentsMatchTheFixtures) {
    FakeIngest ingest;
    EXPECT_EQ(ResultOf(HandleDocumentsList(ingest)),
              LoadFixture("guidance-documents.json")["result"]);
    EXPECT_EQ(DocumentJson(ReadyDocument()), LoadFixture("guidance-document.json")["params"]);
    EXPECT_EQ(ProgressJson({IndexingDocument().id, "preparing", 120, 310}),
              LoadFixture("guidance-progress.json")["params"]);

    const auto added = HandleDocumentsAdd(
        ingest, json{{"paths",
                      {"C:\\Guidelines\\PMR local pathway.txt",
                       "C:\\Guidelines\\BSR gout guideline 2017.md", "C:\\Guidelines\\scan.pdf"}}});
    EXPECT_EQ(ResultOf(added), LoadFixture("guidance-documents-add.json")["result"]);
    ASSERT_EQ(ingest.added.size(), 3u);
    EXPECT_EQ(ingest.added[0].filename(), "PMR local pathway.txt");

    const auto page = HandleDocumentsPage(
        ingest,
        json{{"id", ReadyDocument().id}, {"page", 2}, {"chunkId", "upload:7302914125883421-4"}});
    EXPECT_EQ(ResultOf(page), LoadFixture("guidance-page.json")["result"]);
    ASSERT_EQ(ingest.rendered.size(), 1u);
    EXPECT_EQ(ingest.rendered[0], std::make_pair(2, std::int64_t{4}));
    EXPECT_EQ(ResultOf(HandleDocumentsOpen(ingest, json{{"id", ReadyDocument().id}})),
              LoadFixture("guidance-documents-open.json")["result"]);

    // guidance/documentsChanged goes out only when the ready set changes
    const auto removed = [](clinicavt::guidance::DocumentInfo d) {
        d.state = "removed";
        return d;
    };
    struct Row {
        const char* what;
        clinicavt::guidance::DocumentInfo document;
        bool changes;
    };
    const Row rows[] = {
        {"finished", ReadyDocument(), true},
        {"still indexing", IndexingDocument(), false},
        {"failed", FailedDocument(), false},
        {"finished, then removed", removed(ReadyDocument()), true},
        {"removed while indexing", removed(IndexingDocument()), false},
    };
    for (const auto& row : rows) {
        EXPECT_EQ(ChangesReadySet(row.document), row.changes) << row.what;
    }
}

TEST(Handlers, DocumentsRefuseBadParamsAndUnknownIds) {
    FakeIngest ingest;
    const auto known = ReadyDocument().id;
    struct Case {
        const char* what;
        std::variant<json, Error> outcome;
    };
    const Case cases[] = {
        {"add without paths", HandleDocumentsAdd(ingest, json::object())},
        {"add no paths", HandleDocumentsAdd(ingest, json{{"paths", json::array()}})},
        {"add a path that is not text", HandleDocumentsAdd(ingest, json{{"paths", {1}}})},
        {"remove a string id", HandleDocumentsRemove(ingest, json{{"id", "7"}})},
        {"remove an unknown id", HandleDocumentsRemove(ingest, json{{"id", 1}})},
        {"page without a chunk", HandleDocumentsPage(ingest, json{{"id", known}, {"page", 2}})},
        {"page below zero",
         HandleDocumentsPage(ingest, json{{"id", known}, {"page", -1}, {"chunkId", "upload:1-0"}})},
        {"page of an unknown id",
         HandleDocumentsPage(ingest, json{{"id", 1}, {"page", 0}, {"chunkId", "upload:1-0"}})},
        {"page of an unnumbered chunk",
         HandleDocumentsPage(ingest, json{{"id", known}, {"page", 0}, {"chunkId", "upload:1-x"}})},
        {"open an unknown id", HandleDocumentsOpen(ingest, json{{"id", 1}})},
    };
    for (const auto& c : cases) {
        ASSERT_TRUE(std::holds_alternative<Error>(c.outcome)) << c.what;
        EXPECT_EQ(std::get<Error>(c.outcome).code, kInvalidParams) << c.what;
    }
    EXPECT_TRUE(ingest.added.empty());
    EXPECT_TRUE(ingest.removed.empty());
    EXPECT_TRUE(ingest.rendered.empty());

    EXPECT_TRUE(std::holds_alternative<json>(HandleDocumentsRemove(ingest, json{{"id", known}})));
    EXPECT_EQ(ingest.removed, std::vector<std::int64_t>{known});
}

TEST(Handlers, GuidanceSearchRunsAStoredNoteOrFreeText) {
    SessionStoreFixture fixture;
    const auto id = fixture.AddFinalisedSession();
    fixture.store->SaveDocument(
        id, clinicavt::store::DocumentKind::kNote,
        {.text = "Six weeks of synovitis in the small joints of both hands.",
         .style = "prose",
         .detail = "standard"});
    EchoRetriever retriever;
    Sent sent;
    clinicavt::guidance::GuidanceLane lane(retriever);

    // A stored note: the reply is immediate, the results name the session
    const auto outcome = HandleGuidanceSearch(*fixture.store, lane, json{{"id", id}}, sent.Sink());
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    EXPECT_EQ(ResultOf(outcome), json::object());
    ASSERT_TRUE(sent.WaitFor(1));
    EXPECT_EQ(sent.all[0].first, "guidance/ready");
    EXPECT_EQ(sent.all[0].second["id"], id);
    EXPECT_EQ(sent.all[0].second.at("storeError"), nullptr);
    EXPECT_EQ(sent.all[0].second.at("stale"), false);
    EXPECT_EQ(sent.all[0].second["shown"][0]["trigger"],
              "Six weeks of synovitis in the small joints of both hands.");
    ASSERT_EQ(retriever.searches.size(), 1u);
    EXPECT_EQ(retriever.searches[0].second, kGuidanceLimit);

    // Free text with a limit is searched as a query and belongs to no session
    ASSERT_TRUE(std::holds_alternative<json>(HandleGuidanceSearch(
        *fixture.store, lane, json{{"text", "Chest pain on exertion."}, {"limit", 5}},
        sent.Sink())));
    ASSERT_TRUE(sent.WaitFor(2));
    EXPECT_EQ(sent.all[1].first, "guidance/ready");
    EXPECT_TRUE(sent.all[1].second["id"].is_null());
    EXPECT_EQ(sent.all[1].second.at("storeError"), nullptr);
    EXPECT_EQ(sent.all[1].second.at("stale"), nullptr) << "no note, nothing to be stale against";
    ASSERT_EQ(retriever.searches.size(), 2u);
    EXPECT_EQ(retriever.searches[1], (std::pair<std::string, int>{"Chest pain on exertion.", 5}));
    EXPECT_EQ(retriever.modes[1], clinicavt::guidance::SearchMode::kQuery);
}

TEST(Handlers, GuidanceSearchRunsTypedTextAsANoteOnRequest) {
    SessionStoreFixture fixture;
    EchoRetriever retriever;
    Sent sent;
    clinicavt::guidance::GuidanceLane lane(retriever);

    const auto outcome = HandleGuidanceSearch(
        *fixture.store, lane, json{{"text", "Chest pain on exertion."}, {"mode", "note"}},
        sent.Sink());
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    ASSERT_TRUE(sent.WaitFor(1));
    EXPECT_EQ(
        retriever.modes,
        (std::vector<clinicavt::guidance::SearchMode>{clinicavt::guidance::SearchMode::kNote}));
}

TEST(Handlers, GuidanceSearchRefusesBadParamsAndAMissingNote) {
    SessionStoreFixture fixture;
    const auto without_note = fixture.AddFinalisedSession();
    EchoRetriever retriever;
    Sent sent;
    clinicavt::guidance::GuidanceLane lane(retriever);

    struct Case {
        json params;
        int code;
    };
    const Case cases[] = {
        {json::object(), kInvalidParams},
        {json{{"text", 5}}, kInvalidParams},
        {json{{"text", "Chest pain."}, {"limit", 0}}, kInvalidParams},
        {json{{"text", "Chest pain."}, {"limit", 21}}, kInvalidParams},
        {json{{"text", "Chest pain."}, {"mode", "loud"}}, kInvalidParams},
        {json{{"text", ""}}, kSessionError},
        {json{{"id", "nope"}}, kSessionError},
        {json{{"id", without_note}}, kSessionError},
    };
    for (const auto& c : cases) {
        const auto outcome = HandleGuidanceSearch(*fixture.store, lane, c.params, sent.Sink());
        ASSERT_TRUE(std::holds_alternative<Error>(outcome)) << c.params.dump();
        EXPECT_EQ(std::get<Error>(outcome).code, c.code) << c.params.dump();
    }
    EXPECT_TRUE(retriever.searches.empty());
    EXPECT_TRUE(sent.all.empty());
}

TEST(Handlers, SessionGuidanceReadsTheStoredRecordAndItsStaleness) {
    SessionStoreFixture fixture;
    const auto id = fixture.AddFinalisedSession();
    EXPECT_EQ(ResultOf(HandleSessionGuidance(*fixture.store, json{{"id", id}})),
              (json{{"guidance", nullptr}}));

    fixture.store->SaveDocument(id, clinicavt::store::DocumentKind::kNote, {.text = "note"});
    const json expected = LoadFixture("session-guidance.json")["result"];
    json record = expected["guidance"];
    record.erase("stale");
    fixture.store->SaveDocument(id, clinicavt::store::DocumentKind::kGuidance,
                                {.text = record.dump()});

    EXPECT_EQ(ResultOf(HandleSessionGuidance(*fixture.store, json{{"id", id}})), expected);

    fixture.store->EditDocument(id, clinicavt::store::DocumentKind::kNote, "edited");
    EXPECT_TRUE(
        ResultOf(HandleSessionGuidance(*fixture.store, json{{"id", id}}))["guidance"]["stale"]);

    // A search after a rewrite replaces the record and it reads fresh again
    fixture.store->SaveDocument(id, clinicavt::store::DocumentKind::kNote, {.text = "regenerated"});
    const auto note = fixture.store->ReadDocument(id, clinicavt::store::DocumentKind::kNote);
    Sent sent;
    clinicavt::guidance::Results second;
    second.considered = 1;
    GuidanceSearchRequest(*fixture.store, id, note, 3, sent.Sink()).on_ready(second);
    const json again =
        ResultOf(HandleSessionGuidance(*fixture.store, json{{"id", id}}))["guidance"];
    EXPECT_FALSE(again["stale"]);
    EXPECT_EQ(again["noteRevision"], note.revision);
    EXPECT_EQ(again["considered"], 1);

    // Without an ingest, documents are never reported changed. With one, the record searched
    // no added documents and one is now ready, so they are reported changed
    EXPECT_FALSE(again["documentsChanged"]);
    FakeIngest ingest;
    const json compared =
        ResultOf(HandleSessionGuidance(*fixture.store, json{{"id", id}}, &ingest))["guidance"];
    EXPECT_TRUE(compared["documentsChanged"]);

    for (const char* broken : {"{ not json", R"({"version": 99})", R"({"shown": [{"text": 5}]})"}) {
        fixture.store->SaveDocument(id, clinicavt::store::DocumentKind::kGuidance,
                                    {.text = broken});
        EXPECT_EQ(ResultOf(HandleSessionGuidance(*fixture.store, json{{"id", id}})),
                  (json{{"guidance", nullptr}}))
            << broken;
    }

    const auto unknown = HandleSessionGuidance(*fixture.store, json{{"id", "nope"}});
    ASSERT_TRUE(std::holds_alternative<Error>(unknown));
    EXPECT_EQ(std::get<Error>(unknown).code, kSessionError);
}

// A full or failing disk reaches the shell in the shape both sides check
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

}  // namespace
}  // namespace clinicavt::ipc
