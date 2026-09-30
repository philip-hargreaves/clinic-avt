#include "core/session/enrolment.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>

#include "session_test_harness.hpp"

namespace clinicavt::session {
namespace {

using namespace test_harness;

// A diariser that cannot embed a voice
struct PrintlessDiariser : FakeDiariser {
    diar::IVoiceprints* Voiceprints() override {
        return nullptr;
    }
};

struct EnrolRig {
    SourceFactory factory = FactoryFor(Script::kStreamUntilStopped);
    FakeVad vad;
    RecordingEvents events;

    std::optional<std::tuple<bool, std::string, double>> Done() {
        events.WaitUntil([&] { return events.enrol_done.has_value(); });
        const std::lock_guard<std::mutex> lock(events.mutex);
        return events.enrol_done;
    }
};

TEST(Enrolment, AFullWindowOfSpeechReplacesTheAnchor) {
    EnrolRig rig;
    FakeDiariser diariser;
    Enrolment enrolment(rig.factory, rig.vad, diariser, rig.events);

    ASSERT_TRUE(enrolment.Start(0.3, {}, 0.1));
    const auto done = rig.Done();
    enrolment.Join();

    ASSERT_TRUE(done.has_value());
    EXPECT_TRUE(std::get<0>(*done)) << std::get<1>(*done);
    EXPECT_GE(std::get<2>(*done), 0.1);
    EXPECT_EQ(diariser.replaced, diariser.voiceprint);
    EXPECT_GT(diariser.replaced_at, 1'700'000'000u) << "stamped with the wall clock";
    EXPECT_FALSE(enrolment.Running());
}

TEST(Enrolment, OneRunsAtATime) {
    EnrolRig rig;
    FakeDiariser diariser;
    Enrolment enrolment(rig.factory, rig.vad, diariser, rig.events);

    ASSERT_TRUE(enrolment.Start(60.0, {}, 0.1));
    EXPECT_TRUE(enrolment.Running());
    EXPECT_FALSE(enrolment.Start(60.0, {}, 0.1));
    ASSERT_TRUE(rig.events.WaitUntil([&] {
        return !rig.events.enrol_progress.empty() &&
               rig.events.enrol_progress.back().speech_s >= 0.2;
    }));
    enrolment.Finish();
    const auto done = rig.Done();
    enrolment.Join();
    ASSERT_TRUE(done.has_value());
    EXPECT_TRUE(std::get<0>(*done)) << "finished early with enough speech";
}

TEST(Enrolment, WhatCannotMakeAPrintLeavesTheAnchorAlone) {
    struct Row {
        const char* name;
        double min_speech_s;
        bool empty_print;
        bool printless;
        const char* why_starts;
    };
    const std::vector<Row> rows = {
        {"too little speech", 60.0, false, false, "not enough clear speech"},
        {"an empty print", 0.1, true, false, "could not build a voiceprint"},
        {"no voiceprint capability", 0.1, false, true, "could not build a voiceprint"},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        EnrolRig rig;
        FakeDiariser plain;
        PrintlessDiariser printless;
        FakeDiariser& diariser = row.printless ? printless : plain;
        if (row.empty_print) diariser.voiceprint.clear();
        Enrolment enrolment(rig.factory, rig.vad, diariser, rig.events);

        ASSERT_TRUE(enrolment.Start(0.3, {}, row.min_speech_s));
        const auto done = rig.Done();
        enrolment.Join();

        ASSERT_TRUE(done.has_value());
        EXPECT_FALSE(std::get<0>(*done));
        EXPECT_EQ(std::get<1>(*done).rfind(row.why_starts, 0), 0u) << std::get<1>(*done);
        EXPECT_TRUE(diariser.replaced.empty());
    }
}

TEST(Enrolment, AMicrophoneThatCannotOpenIsReported) {
    EnrolRig rig;
    rig.factory = [](const std::optional<ReplaySpec>&,
                     const std::string&) -> std::unique_ptr<audio::IAudioSource> {
        throw std::runtime_error("no device");
    };
    FakeDiariser diariser;
    Enrolment enrolment(rig.factory, rig.vad, diariser, rig.events);

    ASSERT_TRUE(enrolment.Start(0.3, {}, 0.1));
    const auto done = rig.Done();
    enrolment.Join();

    ASSERT_TRUE(done.has_value());
    EXPECT_FALSE(std::get<0>(*done));
    EXPECT_EQ(std::get<1>(*done), "microphone unavailable: no device");
    EXPECT_TRUE(diariser.replaced.empty());
}

}  // namespace
}  // namespace clinicavt::session
