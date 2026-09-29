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

TEST(SessionController, EnrolmentFinishedEarlyReplacesTheAnchorAndStoresNothing) {
    Rig rig;
    auto controller = rig.Make(Script::kStreamUntilStopped);

    ASSERT_TRUE(controller.StartEnrolment(120.0, {}, 0.1));  // a cap far beyond the test
    ASSERT_TRUE(rig.events.WaitUntil([&] {
        return !rig.events.enrol_progress.empty() &&
               rig.events.enrol_progress.back().speech_s >= 0.2;
    })) << "the level and speech so far are reported";
    controller.FinishEnrolment();
    const auto done = WaitEnrol(rig);

    ASSERT_TRUE(done.has_value());
    EXPECT_TRUE(std::get<0>(*done)) << std::get<1>(*done);
    EXPECT_GE(std::get<2>(*done), 0.1);
    EXPECT_LT(std::get<2>(*done), 60.0) << "stopped at Finish, not at the cap";
    EXPECT_EQ(rig.diariser.replaced, rig.diariser.voiceprint);
    EXPECT_GT(rig.diariser.replaced_at, 1'700'000'000u) << "stamped with the wall clock";
    EXPECT_GE(rig.diariser.embedded_frames, 1600u) << "the gated speech reached the embedder";
    EXPECT_TRUE(rig.events.levels.empty()) << "not as session levels";
    EXPECT_TRUE(rig.store.Calls().empty()) << "an enrolment stores nothing";
}

TEST(SessionController, AnEnrolmentThatDoesNotCompleteLeavesTheAnchorAlone) {
    {
        Rig rig;
        auto controller = rig.Make(Script::kStreamUntilStopped);
        ASSERT_TRUE(controller.Start());
        EXPECT_FALSE(controller.StartEnrolment(0.5)) << "the microphone is the session's";
        controller.Stop();

        ASSERT_TRUE(controller.StartEnrolment(2.0, {}, 0.1));
        EXPECT_FALSE(controller.Start()) << "the microphone is the enrolment's";
        controller.CancelEnrolment();
        const auto done = WaitEnrol(rig);
        ASSERT_TRUE(done.has_value());
        EXPECT_FALSE(std::get<0>(*done));
        EXPECT_EQ(std::get<1>(*done), "cancelled");
        EXPECT_TRUE(rig.diariser.replaced.empty()) << "a cancelled enrolment changes nothing";
        ASSERT_TRUE(controller.Start()) << "and the microphone is free again";
        controller.Stop();
    }

    Rig rig;
    auto controller = rig.Make(Script::kDieAfterAudio);
    ASSERT_TRUE(controller.StartEnrolment(5.0, {}, 0.01));
    const auto done = WaitEnrol(rig);
    ASSERT_TRUE(done.has_value());
    EXPECT_FALSE(std::get<0>(*done));
    EXPECT_EQ(std::get<1>(*done), "microphone unplugged");
    EXPECT_TRUE(rig.diariser.replaced.empty());
}

}  // namespace
}  // namespace clinicavt::session
