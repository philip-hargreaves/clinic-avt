#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "core/session/session_controller.hpp"
#include "session_test_harness.hpp"

namespace clinicavt::session {
namespace {

using namespace test_harness;

// Import

TEST(SessionController, AnImportFinalisesAsAStopDoesButNeverTeachesThePrint) {
    Rig rig;
    rig.diariser.clusters = 2;
    rig.diariser.similarities = {0.2, 0.8};
    rig.diariser.decode_halves = true;
    const std::vector<float> recording(kTwoTurnFrames, 0.1F);
    ImportLog log;
    {
        auto controller = rig.Make(Script::kNeverAudio, {.writer = true});
        EXPECT_EQ(controller.Import(Reads(recording), "2026-09-26T13:05:00Z", false, log.Report()),
                  "s1")
            << "the session is begun before the answer";
        ASSERT_TRUE(log.Wait());
        EXPECT_EQ(log.done, (std::pair<store::SessionId, std::string>{"s1", ""}));
        EXPECT_FALSE(controller.Running()) << "the slot is free once done is reported";
        EXPECT_FALSE(controller.Importing());
        EXPECT_EQ(controller.LastFinalised(), "s1");
        ASSERT_TRUE(rig.events.WaitForNote());
        EXPECT_EQ(rig.events.note_ready, "the clinical note");
    }

    using enum ImportStage;
    EXPECT_EQ(log.progress, (std::vector<std::pair<ImportStage, int>>{{kReading, 2},
                                                                      {kReading, 5},
                                                                      {kSpeech, 10},
                                                                      {kSpeech, 15},
                                                                      {kTranscribing, 55},
                                                                      {kTranscribing, 95},
                                                                      {kFinalising, 95},
                                                                      {kFinalising, 100}}))
        << "one figure across the stages, each span's end in order";
    EXPECT_EQ(ImportPercent(kTranscribing, 1.2), 95) << "a stage never runs past its band";
    EXPECT_EQ(ImportPercent(kSpeech, -0.1), 5);
    EXPECT_EQ(rig.diariser.speech_passes, 1);
    EXPECT_EQ(rig.store.last_started_at, "2026-09-26T13:05:00Z");
    EXPECT_FALSE(rig.store.last_retain);
    EXPECT_EQ(rig.store.sweeps, 1) << "the previous consultation is left, as at start";
    EXPECT_TRUE(rig.store.frames.empty()) << "the file is the durable copy, nothing is appended";
    EXPECT_EQ(rig.store.Calls(),
              (std::vector<std::string>{"begin s1", "replace s1", "finalise s1", "note s1"}));
    EXPECT_EQ(rig.events.progress, (std::vector<std::string>{"transcript", "speakers", "turns"}))
        << "the transcript stage shows while the whole file decodes";
    EXPECT_EQ(rig.diariser.advances.load(), 0) << "no capture ticks";
    EXPECT_EQ(rig.diariser.settles, 1) << "the clip cuts come from the settle pass";
    EXPECT_EQ(rig.diariser.settled_frames, recording.size());
    EXPECT_EQ(rig.diariser.audio_frames, recording.size());
    EXPECT_GE(rig.writer.prepares.load(), 1) << "the note model warms alongside the finalise";

    ASSERT_EQ(rig.store.turns.size(), 2u);
    EXPECT_EQ(rig.store.turns[0].speaker, "patient");
    EXPECT_EQ(rig.store.turns[1].speaker, "doctor") << "the print still names the roles";
    EXPECT_EQ(rig.diariser.voiceprint_cluster, -1);
    EXPECT_EQ(rig.diariser.accruals.load(), 0) << "an accepted note teaches the print nothing";
}

TEST(SessionController, AnImportIsRefusedWhileTheMicrophoneIsInUse) {
    Rig rig;
    const std::vector<float> recording(kTwoTurnFrames, 0.1F);
    auto controller = rig.Make(Script::kStreamUntilStopped);

    ASSERT_TRUE(controller.Start());
    EXPECT_EQ(controller.Import(Reads(recording), "", true, {}), "")
        << "a consultation is recording";
    EXPECT_TRUE(controller.Running());
    controller.Stop();

    ASSERT_TRUE(controller.StartEnrolment(2.0, {}, 0.1));
    EXPECT_EQ(controller.Import(Reads(recording), "", true, {}), "") << "an enrolment is recording";
    controller.CancelEnrolment();
    ASSERT_TRUE(WaitEnrol(rig).has_value());

    rig.store.refuse_begin = true;
    EXPECT_EQ(controller.Import(Reads(recording), "", true, {}), "") << "the store refused";
    rig.store.refuse_begin = false;
    ImportLog log;
    EXPECT_EQ(controller.Import(Reads(recording), "", true, log.Report()), "s2")
        << "and a refusal leaves the slot free";
    ASSERT_TRUE(log.Wait());
    EXPECT_EQ(rig.store.begins, 2) << "no refusal began a session";
    EXPECT_EQ(rig.store.last_started_at, "") << "empty dates it now";
}

TEST(SessionController, WhileAnImportRunsTheSlotIsItsAndStopWaitsForIt) {
    Rig rig;
    rig.diariser.decode_halves = true;
    std::atomic<bool> midway{false};
    std::atomic<bool> release{false};
    rig.diariser.mid_settle = [&] {
        midway = true;
        (void)WaitFor([&] { return release.load(); });
    };
    ImportLog log;
    auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});
    ASSERT_EQ(
        controller.Import(Reads(std::vector<float>(kTwoTurnFrames, 0.1F)), "", true, log.Report()),
        "s1");
    ASSERT_TRUE(WaitFor([&] { return midway.load(); }));

    EXPECT_TRUE(controller.Importing());
    EXPECT_TRUE(controller.Busy()) << "a note model switch and a delete wait";
    EXPECT_FALSE(controller.Start()) << "the slot is the import's";
    EXPECT_EQ(controller.Import(Reads(std::vector<float>(kTwoTurnFrames, 0.1F)), "", true, {}), "");
    EXPECT_FALSE(controller.StartEnrolment(2.0, {}, 0.1));
    EXPECT_FALSE(controller.Open("s1")) << "nor can a review open meanwhile";

    // A closing shell stops the controller: the import finishes and is kept
    std::thread closing([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        release = true;
    });
    controller.Stop();
    closing.join();
    {
        const std::lock_guard<std::mutex> lock(log.mutex);
        ASSERT_TRUE(log.done.has_value()) << "Stop returned only once the import was done";
        EXPECT_EQ(log.done->second, "");
    }
    const auto calls = rig.store.Calls();
    EXPECT_NE(std::find(calls.begin(), calls.end(), "finalise s1"), calls.end());
}

TEST(SessionController, ACancelledImportIsErasedAndFreesTheSlotAtOnce) {
    Rig rig;
    rig.diariser.decode_halves = true;
    std::atomic<bool> midway{false};
    std::atomic<bool> cancelled{false};
    rig.diariser.mid_settle = [&] {
        midway = true;
        (void)WaitFor([&] { return cancelled.load(); });
    };
    ImportLog log;
    auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});
    ASSERT_EQ(
        controller.Import(Reads(std::vector<float>(kTwoTurnFrames, 0.1F)), "", true, log.Report()),
        "s1");
    ASSERT_TRUE(WaitFor([&] { return midway.load(); }));
    controller.Cancel();
    EXPECT_TRUE(controller.Importing()) << "the import's own thread erases it";
    cancelled = true;
    ASSERT_TRUE(log.Wait());

    EXPECT_EQ(log.done, (std::pair<store::SessionId, std::string>{"s1", kImportCancelled}));
    ASSERT_FALSE(log.progress.empty());
    EXPECT_EQ(log.progress.back(), std::make_pair(ImportStage::kTranscribing, 55))
        << "the second half never decoded, and nothing finalised";
    EXPECT_EQ(rig.store.Calls(), (std::vector<std::string>{"begin s1", "cancel s1"}));
    EXPECT_EQ(controller.LastFinalised(), "");
    EXPECT_EQ(rig.events.progress, (std::vector<std::string>{"transcript"}))
        << "no speakers or note follow";
    EXPECT_FALSE(controller.Busy());

    // The cancel was import-only: the next recording stops and is kept
    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(rig.WaitForFrames(1));
    controller.Stop();
    EXPECT_EQ(controller.LastFinalised(), "s2");
}

}  // namespace
}  // namespace clinicavt::session
