#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "adapters/models/model_store.hpp"
#include "adapters/note/worker_note_writer.hpp"

namespace clinicavt::note {
namespace {

using Phase = NoteModelState::Phase;

// A store with a note model per tier. The fake host loads none of them
struct TieredStore {
    std::filesystem::path root;

    TieredStore() {
        root =
            std::filesystem::temp_directory_path() /
            ("clinicavt-lane-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        Stage("qwen3.5-9b-int4", "Qwen3.5 9B", "default");
        Stage("qwen3.6-35b-a3b-int4", "Qwen3.6 35B", "accuracy");
        Stage("qwen-broken-int4", "Broken", "constrained");
    }

    ~TieredStore() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void Stage(const std::string& id, const std::string& name, const std::string& tier) const {
        std::filesystem::create_directories(root / id);
        std::ofstream(root / id / "manifest.json")
            << R"({"manifestVersion": 1, "id": ")" << id << R"(", "name": ")" << name
            << R"(", "task": "note", "tier": ")" << tier
            << R"(", "licence": "Apache-2.0", "runtime": {"device": "GPU"},)"
            << R"( "files": {"model.xml": "00"}})";
    }
};

struct Transitions {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<NoteModelState> seen;

    INoteTiers::Listener Listener() {
        return [this](const NoteModelState& state) {
            std::lock_guard<std::mutex> lock(mutex);
            seen.push_back(state);
            changed.notify_all();
        };
    }

    // The first state at or after `from` in the given phase, within 5 s
    bool WaitFor(Phase phase, std::size_t from = 0) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&] {
            for (std::size_t i = from; i < seen.size(); ++i) {
                if (seen[i].phase == phase) return true;
            }
            return false;
        });
    }

    std::vector<NoteModelState> Snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return seen;
    }
};

TEST(NoteLane, ConfiguringATierLoadsItAtOnceAndTheHostServesIt) {
    TieredStore staged;
    const models::ModelStore store(staged.root);
    Transitions seen;
    WorkerNoteWriter lane(CLINICAVT_FAKE_NOTE_HOST, staged.root, staged.root, &store, "default",
                          seen.Listener());
    const auto start = lane.State();
    EXPECT_EQ(start.phase, Phase::kIdle);
    EXPECT_EQ(start.tier, "default");
    EXPECT_EQ(start.id, "qwen3.5-9b-int4");
    EXPECT_EQ(start.name, "Qwen3.5 9B");

    const auto reply = lane.Configure("accuracy");

    EXPECT_EQ(reply.tier, "accuracy");
    EXPECT_EQ(reply.id, "qwen3.6-35b-a3b-int4");
    EXPECT_EQ(reply.phase, Phase::kLoading) << "warm on switch: the load starts in the call";
    ASSERT_TRUE(seen.WaitFor(Phase::kReady));
    const auto states = seen.Snapshot();
    EXPECT_EQ(states.front().phase, Phase::kIdle) << "the switch is announced before the load";
    EXPECT_EQ(states.back().tier, "accuracy");
    EXPECT_EQ(states.back().name, "Qwen3.6 35B");
    EXPECT_GT(states.back().seconds, 0.0);

    EXPECT_EQ(lane.Configure("accuracy").phase, Phase::kReady);
    EXPECT_EQ(seen.Snapshot().size(), states.size()) << "the same tier again: no respawn";

    EXPECT_EQ(lane.Write({{0, 16000, "doctor", "hello"}}, {}, nullptr),
              "A note from qwen3.6-35b-a3b-int4");
    EXPECT_EQ(lane.State().phase, Phase::kReady);
    // The case summary goes through the host like the label: one call, one text
    EXPECT_EQ(lane.WriteSummary("the note"), "A summary from qwen3.6-35b-a3b-int4");
    EXPECT_THROW(lane.WriteSummary(""), std::runtime_error);
}

TEST(NoteLane, WhatTheLaneCannotServeFailsLoudlyAndChangesNothing) {
    TieredStore staged;
    const models::ModelStore store(staged.root);
    Transitions seen;
    WorkerNoteWriter lane(CLINICAVT_FAKE_NOTE_HOST, staged.root, staged.root, &store, "default",
                          seen.Listener());

    EXPECT_THROW(lane.Write({}, {}, nullptr), std::runtime_error) << "an empty transcript";

    try {
        lane.Configure("fast");
        FAIL() << "nothing claims note/fast";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("qwen3.5-9b-int4"), std::string::npos)
            << "names what is staged: " << e.what();
    }
    EXPECT_EQ(lane.State().tier, "default") << "a refused switch changes nothing";

    lane.Configure("constrained");
    ASSERT_TRUE(seen.WaitFor(Phase::kFailed));
    const auto failed = seen.Snapshot().back();
    EXPECT_EQ(failed.tier, "constrained");
    EXPECT_EQ(failed.detail, "no such device") << "a failed load reports its reason";
    const auto from = seen.Snapshot().size();
    lane.Configure("default");
    ASSERT_TRUE(seen.WaitFor(Phase::kReady, from)) << "the previous tier loads again";
    EXPECT_EQ(seen.Snapshot().back().id, "qwen3.5-9b-int4");

    WorkerNoteWriter missing("C:/nowhere/clinicavt_note_host.exe", staged.root, staged.root);
    EXPECT_THROW(missing.Write({{0, 16000, "doctor", "hello"}}, {}, nullptr), std::runtime_error)
        << "a missing host";
}

// A load cannot be cancelled, so a switch waits for it rather than killing the
// host, and the stuck-host probe leaves it alone
TEST(NoteLane, ASwitchDuringALoadIsRefusedUntilTheLoadSettles) {
    TieredStore staged;
    std::filesystem::remove_all(staged.root / "qwen3.6-35b-a3b-int4");
    staged.Stage("qwen-slow-int4", "Slow", "accuracy");
    const models::ModelStore store(staged.root);
    Transitions seen;
    WorkerNoteWriter lane(CLINICAVT_FAKE_NOTE_HOST, staged.root, staged.root, &store, "default",
                          seen.Listener());
    lane.Configure("accuracy");

    EXPECT_THROW(lane.Configure("default"), std::logic_error);
    EXPECT_EQ(lane.State().tier, "accuracy");
    EXPECT_FALSE(lane.CheckForStuckHost()) << "a load is never probed";
    EXPECT_EQ(lane.State().phase, Phase::kLoading) << "the host is left to finish it";

    ASSERT_TRUE(seen.WaitFor(Phase::kReady));
    const auto from = seen.Snapshot().size();
    lane.Configure("default");
    ASSERT_TRUE(seen.WaitFor(Phase::kReady, from));
    EXPECT_EQ(seen.Snapshot().back().id, "qwen3.5-9b-int4");
}

// The host reads nothing while prefilling, so a second large prefill would block the capture
// thread. The lane skips it
TEST(NoteLane, APrefillWaitsForTheLastOneRatherThanBlockingTheCaller) {
    TieredStore staged;
    const models::ModelStore store(staged.root);
    Transitions seen;
    WorkerNoteWriter lane(CLINICAVT_FAKE_NOTE_HOST, staged.root, staged.root, &store, "default",
                          seen.Listener());
    lane.Configure("accuracy");
    ASSERT_TRUE(seen.WaitFor(Phase::kReady));
    std::vector<asr::Turn> transcript;
    for (int i = 0; i < 400; ++i) {
        transcript.push_back({0, 16000, "doctor", std::string(400, 'a')});
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) lane.Prefill(transcript, {});

    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(1000));
}

TEST(NoteLane, AHealthyHostAskedIfStuckExitsAndServesTheNextNote) {
    TieredStore staged;
    const models::ModelStore store(staged.root);
    Transitions seen;
    WorkerNoteWriter lane(CLINICAVT_FAKE_NOTE_HOST, staged.root, staged.root, &store, "default",
                          seen.Listener());
    lane.Configure("accuracy");
    ASSERT_TRUE(seen.WaitFor(Phase::kReady));

    EXPECT_FALSE(lane.CheckForStuckHost());

    EXPECT_EQ(lane.State().phase, Phase::kIdle);
    EXPECT_EQ(lane.Write({{0, 16000, "doctor", "hello"}}, {}, nullptr),
              "A note from qwen3.6-35b-a3b-int4");
}

}  // namespace
}  // namespace clinicavt::note
