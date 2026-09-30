#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "adapters/ipc/handlers.hpp"
#include "handler_test_support.hpp"

namespace clinicavt::ipc {
namespace {

using namespace handler_test;

// Seeds once, lists as samples, clears without touching the real session
TEST(Handlers, TheSampleYearSeedsOnceAndClearsCleanly) {
    using clinicavt::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto real = fixture.AddFinalisedSession();

    const auto seeded = HandleDemoSeed(fixture.demo);
    ASSERT_TRUE(std::holds_alternative<json>(seeded))
        << (std::holds_alternative<Error>(seeded) ? std::get<Error>(seeded).message : "");
    EXPECT_EQ(MakeResult(std::int64_t{30}, ResultOf(seeded)), LoadFixture("demo-seed.json"));
    const auto again = HandleDemoSeed(fixture.demo);
    ASSERT_TRUE(std::holds_alternative<json>(again));
    EXPECT_EQ(ResultOf(again)["added"], 0) << "already seeded is a no-op, not an error";

    const json sessions = HandleSessionList(fixture.records)["sessions"];
    EXPECT_EQ(sessions.size(), 7u);
    std::size_t samples = 0;
    for (const auto& s : sessions) {
        if (s["sample"].get<bool>()) {
            samples += 1;
            EXPECT_FALSE(s["label"].get<std::string>().empty());
            EXPECT_GT(s["audioSeconds"].get<double>(), 300.0);
        } else {
            EXPECT_EQ(s["id"], real);
        }
    }
    EXPECT_EQ(samples, 6u);
    const json entries = HandleReflectionList(fixture.reflections)["reflections"];
    ASSERT_EQ(entries.size(), 6u);
    for (const auto& e : entries) EXPECT_TRUE(e["sample"].get<bool>());
    const auto sample = entries[0]["id"].get<std::string>();
    EXPECT_FALSE(fixture.store->ReadDocument(sample, DocumentKind::kNote).text.empty());
    EXPECT_FALSE(fixture.store->ReadDocument(sample, DocumentKind::kPatient).text.empty());

    EXPECT_EQ(MakeResult(std::int64_t{31}, HandleDemoClear(fixture.demo)),
              LoadFixture("demo-clear.json"));
    const json left = HandleSessionList(fixture.records)["sessions"];
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0]["id"], real);
    EXPECT_EQ(HandleReflectionList(fixture.reflections)["reflections"].size(), 0u);

    // One erase removes samples and real sessions alike
    ASSERT_TRUE(std::holds_alternative<json>(HandleDemoSeed(fixture.demo)));
    const auto erased =
        HandleSessionDeleteAll(fixture.records, json{{"deleteReflections", true}}, false, false);
    ASSERT_TRUE(std::holds_alternative<json>(erased));
    EXPECT_EQ(ResultOf(erased)["removed"], 7);
    EXPECT_EQ(HandleSessionList(fixture.records)["sessions"].size(), 0u);
}

TEST(Handlers, AStoredSummaryLeavesScrubbedWhateverWasStored) {
    using clinicavt::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto id = fixture.AddFinalisedSession();
    fixture.store->SaveDocument(id, DocumentKind::kSummary,
                                {.text = "A 53-year-old male presented with a swollen elbow."});

    const json got = ResultOf(HandleReflectionGet(fixture.reflections, json{{"id", id}}));
    EXPECT_EQ(got["summary"]["text"], "A male in their fifties presented with a swollen elbow.");
    const json listed = HandleReflectionList(fixture.reflections)["reflections"];
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0]["summary"], "A male in their fifties presented with a swollen elbow.");

    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        fixture.reflections, json{{"id", id}, {"summary", "The patient, aged 67, was seen."}})));
    EXPECT_EQ(fixture.store->ReadDocument(id, DocumentKind::kSummary).text,
              "The patient, in their sixties, was seen.");
}

TEST(Handlers, ReflectionGetUpdateListAndDelete) {
    using clinicavt::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto id = fixture.AddFinalisedSession();
    fixture.store->SaveDocument(id, DocumentKind::kLabel, {.text = "Elbow swelling"});

    // Before anything is written there is only a label, both parts are null and it is unlisted
    json got = ResultOf(HandleReflectionGet(fixture.reflections, json{{"id", id}}));
    EXPECT_EQ(got["label"], "Elbow swelling");
    EXPECT_TRUE(got["summary"].is_null());
    EXPECT_TRUE(got["reflection"].is_null());
    EXPECT_EQ(HandleReflectionList(fixture.reflections)["reflections"].size(), 0u);

    // A summary alone is an entry because the sheet was opened and the writing can follow
    fixture.store->SaveDocument(id, DocumentKind::kSummary,
                                {.text = "A patient in their forties."});
    json listed = HandleReflectionList(fixture.reflections)["reflections"];
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0]["learned"], "");
    EXPECT_EQ(listed[0]["summary"], "A patient in their forties.");
    EXPECT_TRUE(listed[0]["createdAt"].is_string());

    // Three empty answers keep the entry. Only delete removes it
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        fixture.reflections, json{{"id", id}, {"happened", ""}, {"learned", ""}, {"next", ""}})));
    EXPECT_EQ(HandleReflectionList(fixture.reflections)["reflections"].size(), 1u);

    // A first answer creates the entry and a later one merges into it
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        fixture.reflections, json{{"id", id}, {"learned", "check the temperature"}})));
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        fixture.reflections, json{{"id", id}, {"next", "add a red-flag check"}})));
    got = ResultOf(HandleReflectionGet(fixture.reflections, json{{"id", id}}));
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
        fixture.reflections,
        json{{"id", id},
             {"references", json::array({{{"key", "nice:ng100"},
                                          {"reference", "NG100"},
                                          {"title", "Rheumatoid arthritis in adults"},
                                          {"link", "https://www.nice.org.uk/guidance/ng100"},
                                          {"source", "NICE"}},
                                         {{"key", "upload:doc-7"}, {"title", "Leaflet"}}})}})));
    got = ResultOf(HandleReflectionGet(fixture.reflections, json{{"id", id}}));
    ASSERT_EQ(got["reflection"]["references"].size(), 2u);
    EXPECT_EQ(got["reflection"]["references"][0]["reference"], "NG100");
    EXPECT_EQ(got["reflection"]["references"][1]["link"], "") << "missing fields read empty";
    EXPECT_EQ(got["reflection"]["learned"], "check the temperature");
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        fixture.reflections, json{{"id", id}, {"references", json::array()}})));
    got = ResultOf(HandleReflectionGet(fixture.reflections, json{{"id", id}}));
    EXPECT_TRUE(got["reflection"]["references"].empty());
    EXPECT_TRUE(std::holds_alternative<Error>(HandleReflectionUpdate(
        fixture.reflections, json{{"id", id}, {"references", json::array({"NG100"})}})));

    // The same update can correct the summary
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        fixture.reflections, json{{"id", id}, {"summary", "A patient in their forties."}})));
    got = ResultOf(HandleReflectionGet(fixture.reflections, json{{"id", id}}));
    EXPECT_EQ(got["summary"]["text"], "A patient in their forties.");
    EXPECT_TRUE(got["summary"]["editedAt"].is_string());

    // Listed with the card's line
    const json list = HandleReflectionList(fixture.reflections)["reflections"];
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
        HandleReflectionUpdate(fixture.reflections, json{{"id", id}, {"learned", 3}})));
    EXPECT_TRUE(std::holds_alternative<Error>(
        HandleReflectionGet(fixture.reflections, json{{"id", "nope"}})));

    ASSERT_TRUE(std::holds_alternative<json>(
        HandleReflectionDelete(fixture.reflections, json{{"id", id}})));
    got = ResultOf(HandleReflectionGet(fixture.reflections, json{{"id", id}}));
    EXPECT_TRUE(got["reflection"].is_null());
    EXPECT_TRUE(got["summary"].is_null());
    EXPECT_EQ(HandleReflectionList(fixture.reflections)["reflections"].size(), 0u);
}

TEST(Handlers, ReflectionSummaryAndDeleteMatchTheFixtures) {
    using clinicavt::store::DocumentKind;
    ServicesRig rig;
    WireShell shell;
    RegisterMethods(shell.server, rig.Services());
    ASSERT_NO_FATAL_FAILURE(shell.Connect());
    const auto id = rig.fixture.AddFinalisedSession();
    rig.fixture.store->SaveDocument(id, DocumentKind::kNote, {.text = "Swollen left elbow."});

    const json summary = LoadFixture("reflection-summary-request.json");
    EXPECT_EQ(shell.Call(ForSession(summary["request"], id)), summary["response"]);
    ASSERT_TRUE(rig.WaitIdle());

    const json removed = LoadFixture("reflection-delete.json");
    EXPECT_EQ(MakeResult(std::int64_t{57}, ResultOf(HandleReflectionDelete(
                                               rig.fixture.reflections,
                                               ForSession(removed["request"], id)["params"]))),
              removed["response"]);
}

}  // namespace
}  // namespace clinicavt::ipc
