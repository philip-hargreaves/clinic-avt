#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/ipc/handlers.hpp"
#include "core/common/version.hpp"
#include "handler_test_support.hpp"

namespace clinicavt::ipc {
namespace {

using namespace handler_test;

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

    // The reply describes the engine regardless of what the peer sends
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

// Records Configure calls and returns its state member
struct FakeLane : clinicavt::note::INoteTiers {
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

    // There is no note lane when nothing is staged or no note host sits next to the engine
    outcome = HandleNoteTier(nullptr, false, json{{"tier", "default"}});
    ASSERT_TRUE(std::holds_alternative<Error>(outcome));
    EXPECT_EQ(std::get<Error>(outcome).code, kSessionError);

    // "auto" is this machine's tier
    lane.refuse.clear();
    lane.configured.clear();
    outcome = HandleNoteTier(&lane, false, json{{"tier", "auto"}}, "constrained");
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    EXPECT_EQ(lane.configured, std::vector<std::string>{"constrained"});

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

    // Refused for an unknown device or when no model is loaded to move
    moved_to.clear();
    EXPECT_EQ(
        std::get<Error>(HandleAsrDevice(switcher, false, json{{"device", "CPU"}}, notify)).code,
        kInvalidParams);
    EXPECT_TRUE(moved_to.empty());
    EXPECT_EQ(std::get<Error>(HandleAsrDevice({}, false, json{{"device", "GPU"}}, notify)).code,
              kSessionError);
}

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
        {"session/deleteAll",
         [&] { return HandleSessionDeleteAll(fixture.records, json::object(), true, false); }},
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
    EXPECT_EQ(HandleSessionList(fixture.records)["sessions"].size(), 1u);
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

TEST(Handlers, ReadinessNamesTheMissingRolesAsTheFixture) {
    const json fixture = LoadFixture("engine-readiness.json");
    EXPECT_EQ(MakeResult(std::int64_t{5},
                         ReadinessJson(false, true, false, {"asr", "diarisation", "segmentation"})),
              fixture["response"]);
    EXPECT_EQ(ReadinessJson(true, false, true, {})["missing"], json::array())
        << "every role installed, or a stand-in in its place with --scripted";
}

TEST(Handlers, MissingModelsAreNamedInPlainWords) {
    EXPECT_EQ(MissingModelsReason({}), "");
    EXPECT_EQ(MissingModelsReason({"asr"}), "the speech recognition model is not installed");
    EXPECT_EQ(MissingModelsReason({"diarisation", "segmentation"}),
              "the speaker recognition model is not installed")
        << "two models, one thing to a clinician";
    EXPECT_EQ(MissingModelsReason({"asr", "vad", "diarisation"}),
              "the speech recognition, speech detection and speaker recognition models are not "
              "installed");
}

TEST(Handlers, EngineAndAnchorMethodsMatchTheFixtures) {
    ServicesRig rig;
    WireShell shell;
    std::string moved_to;
    RegisterMethods(shell.server, rig.Services([&](const std::string& device, const auto& done) {
        moved_to = device;
        done("");
        return true;
    }));
    ASSERT_NO_FATAL_FAILURE(shell.Connect());

    for (const char* name : {"anchor-clear.json", "anchor-enrol.json", "anchor-enrol-cancel.json",
                             "anchor-enrol-finish.json", "asr-device.json"}) {
        SCOPED_TRACE(name);
        const json fixture = LoadFixture(name);
        EXPECT_EQ(shell.Call(fixture["request"]), fixture["response"]);
    }
    EXPECT_EQ(moved_to, "NPU");
    const auto moved = shell.Notification("asr/device");
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(*moved, LoadFixture("asr-device-notification.json"));

    // Device names, versions and timings differ by machine, so only the fields are compared
    const json metrics = LoadFixture("engine-metrics.json");
    const json reply = shell.Call(metrics["request"]);
    ASSERT_TRUE(reply.contains("result")) << reply.dump();
    EXPECT_EQ(KeysOf(reply["result"]), KeysOf(metrics["response"]["result"]));
    EXPECT_EQ(reply["id"], metrics["response"]["id"]);
}

TEST(Handlers, AudioInputsMatchTheFixture) {
    const std::vector<clinicavt::audio::CaptureDevice> devices{
        {"{0.0.1}.{aa}", "Microphone Array (Realtek(R) Audio)", "Microphone Array", true, false},
        {"{0.0.1}.{bb}", "Headset (H800 Hands-Free)", "Headset", false, true},
    };
    EXPECT_EQ(MakeResult(std::int64_t{45}, HandleAudioInputs(devices)),
              LoadFixture("audio-inputs.json")["response"]);
}

}  // namespace
}  // namespace clinicavt::ipc
