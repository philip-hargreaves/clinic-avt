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

TEST(SessionController, AFailedStartLeavesNoTraceAndTheNextStartWorks) {
    struct Row {
        const char* name;
        Script script;
        bool refuse_begin;
        std::string detail;
        std::vector<std::string> calls;
    };
    const std::vector<Row> rows = {
        {"the source dies first",
         Script::kDieImmediately,
         false,
         "would not open",
         {"begin s1", "cancel s1"}},
        {"no audio before the deadline",
         Script::kNeverAudio,
         false,
         "deadline",
         {"begin s1", "cancel s1"}},
        {"the store refuses the session", Script::kStreamUntilStopped, true, "store", {}},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        Rig rig;
        rig.store.refuse_begin = row.refuse_begin;
        int sources = 0;
        auto controller = rig.Make([&](const std::optional<ReplaySpec>&, const std::string&) {
            return std::make_unique<ScriptedSource>(sources++ == 0 ? row.script
                                                                   : Script::kStreamUntilStopped);
        });

        EXPECT_FALSE(controller.Start());
        EXPECT_FALSE(controller.Running());
        EXPECT_EQ(controller.LastEnd().reason, SourceEndReason::kFailed);
        EXPECT_NE(controller.LastEnd().detail.find(row.detail), std::string::npos)
            << controller.LastEnd().detail;
        EXPECT_EQ(rig.store.Calls(), row.calls)
            << "a session that never produced audio leaves no trace";
        EXPECT_TRUE(rig.events.interruptions.empty());

        rig.store.refuse_begin = false;
        ASSERT_TRUE(controller.Start());
        controller.Stop();
    }
}

TEST(SessionController, EachWayOfEndingASessionHasItsOwnStoreOutcome) {
    enum class Ending { kStop, kCancel, kInterrupted };
    struct Row {
        const char* name;
        Script script;
        Ending ending;
        std::vector<std::string> calls;
        std::optional<SourceEndReason> interruption;
        std::string detail;
        bool store_faults = false;
    };
    const std::vector<Row> rows = {
        {"a stop keeps the attributed recording",
         Script::kStreamUntilStopped,
         Ending::kStop,
         {"begin s1", "replace s1", "finalise s1"}},
        {"a cancel erases it",
         Script::kStreamUntilStopped,
         Ending::kCancel,
         {"begin s1", "cancel s1"}},
        {"a lost device abandons it for recovery",
         Script::kDieAfterAudio,
         Ending::kInterrupted,
         {"begin s1", "abandon s1"},
         SourceEndReason::kDeviceLost,
         "unplugged"},
        {"a throwing source is caught and abandons it",
         Script::kThrowAfterAudio,
         Ending::kInterrupted,
         {"begin s1", "abandon s1"},
         SourceEndReason::kFailed,
         "driver exploded"},
        // One window is shorter than the shortest decodable turn, so there is no transcript
        {"a completed replay waits for the stop",
         Script::kCompleteAfterAudio,
         Ending::kStop,
         {"begin s1", "finalise s1"}},
        {"a completed replay can still be cancelled",
         Script::kCompleteAfterAudio,
         Ending::kCancel,
         {"begin s1", "cancel s1"}},
        {"store faults reach the shell and the session still ends",
         Script::kCompleteAfterAudio,
         Ending::kStop,
         {"begin s1", "finalise s1"},
         std::nullopt,
         "",
         true},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        Rig rig;
        rig.store.fault_audio = row.store_faults;
        rig.store.refuse_outcome = row.store_faults;
        auto controller = rig.Make(row.script);

        ASSERT_TRUE(controller.Start());
        if (row.script == Script::kStreamUntilStopped) {
            ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
        }
        if (row.ending == Ending::kCancel) {
            controller.Cancel();
        } else {
            if (row.ending == Ending::kInterrupted) {
                ASSERT_TRUE(
                    rig.events.WaitUntil([&] { return !rig.events.interruptions.empty(); }));
            }
            controller.Stop();
        }

        EXPECT_FALSE(controller.Running());
        EXPECT_EQ(rig.store.Calls(), row.calls);
        if (row.interruption.has_value()) {
            ASSERT_EQ(rig.events.interruptions.size(), 1u);
            EXPECT_EQ(rig.events.interruptions[0], *row.interruption);
            EXPECT_NE(rig.events.last_detail.find(row.detail), std::string::npos);
        } else {
            EXPECT_TRUE(rig.events.interruptions.empty()) << "the user's own ending";
        }
        if (row.ending == Ending::kCancel) {
            EXPECT_EQ(rig.diariser.calls, 0) << "nothing diarises on cancel";
            EXPECT_TRUE(rig.events.progress.empty()) << "a cancel finalises nothing to report";
            EXPECT_EQ(rig.diariser.takes, 0) << "nothing splices on cancel";
            EXPECT_GE(rig.diariser.discards, 1) << "capture state must not leak";
        }
        if (row.script == Script::kCompleteAfterAudio) {
            EXPECT_EQ(rig.store.lost, 3u) << "loss accounting must reach the store";
            EXPECT_EQ(rig.store.frames.size(), Window().size());
        }
        const auto faults = row.store_faults
                                ? std::vector<std::string>{"audio commit failed", "seal failed"}
                                : std::vector<std::string>{};
        EXPECT_EQ(rig.events.storage_faults, faults);
    }
}

TEST(SessionController, OneSessionAtATimeWithLevelsFromTheFirstWindow) {
    Rig rig;
    int sources = 0;
    auto controller = rig.Make([&](const std::optional<ReplaySpec>&, const std::string&) {
        ++sources;
        return std::make_unique<ScriptedSource>(Script::kStreamUntilStopped);
    });

    controller.Stop();
    EXPECT_TRUE(rig.store.Calls().empty()) << "a stop before start is a no-op";

    ASSERT_TRUE(controller.Start()) << "acked once audio flows";
    EXPECT_TRUE(controller.Running());
    EXPECT_FALSE(controller.Start());
    EXPECT_EQ(rig.store.begins, 1) << "the refused start must not open a second session";
    controller.Stop();

    EXPECT_FALSE(controller.Running());
    ASSERT_FALSE(rig.events.levels.empty());
    EXPECT_NEAR(rig.events.levels.front(), 1.0F, 0.01F);
    EXPECT_TRUE(rig.events.interruptions.empty()) << "a user stop is not an interruption";

    ASSERT_TRUE(controller.Start());
    controller.Stop();
    EXPECT_EQ(sources, 2) << "a restart gets a fresh source";
    EXPECT_EQ(rig.store.begins, 2);
}

TEST(SessionController, AResumedSessionContinuesTheStoredAudioAndSupersedesTheOld) {
    Rig rig;
    rig.store.stored_audio = std::vector<float>(LevelMeter::kWindowFrames * 3, 0.5F);
    const auto combined = rig.store.stored_audio.size() + Window().size();
    auto controller = rig.Make(Script::kCompleteAfterAudio);

    rig.store.refuse_read_audio = true;
    EXPECT_FALSE(controller.Start(std::nullopt, "gone")) << "nothing to resume";
    EXPECT_FALSE(controller.Running());
    EXPECT_TRUE(rig.store.Calls().empty());
    rig.store.refuse_read_audio = false;

    ASSERT_TRUE(controller.Start(std::nullopt, "old-session"));
    ASSERT_TRUE(rig.WaitForFrames(combined));
    controller.Stop();
    const auto index = [&](const std::string& call) {
        const auto calls = rig.store.Calls();
        return std::find(calls.begin(), calls.end(), call) - calls.begin();
    };
    EXPECT_EQ(index("readAudio old-session"), 0);
    EXPECT_EQ(rig.store.frames.size(), combined) << "stored and live audio as one stream";
    EXPECT_EQ(rig.diariser.audio_frames, combined) << "the combined recording is finalised";
    EXPECT_LT(index("finalise s1"), index("delete old-session"))
        << "the old session is superseded once the new one finalises";

    // The new session held all of the old one's audio, so a cancel discards
    // the whole consultation
    ASSERT_TRUE(controller.Start(std::nullopt, "older"));
    controller.Cancel();
    const auto calls = rig.store.Calls();
    ASSERT_NE(std::find(calls.begin(), calls.end(), "delete older"), calls.end());
    EXPECT_LT(index("cancel s2"), index("delete older"));
}

TEST(SessionController, StartOptionsReachTheSourceAndTheRecord) {
    Rig rig;
    std::string mic;
    std::optional<ReplaySpec> replay;
    auto controller =
        rig.Make([&](const std::optional<ReplaySpec>& spec, const std::string& mic_id) {
            mic = mic_id;
            replay = spec;
            return std::make_unique<ScriptedSource>(Script::kStreamUntilStopped);
        });

    ASSERT_TRUE(controller.Start(std::nullopt, {}, true,
                                 {"{0.0.1}.{aa}", "Microphone Array (Cirrus Logic)"}));
    EXPECT_EQ(mic, "{0.0.1}.{aa}") << "the chosen microphone is pinned";
    EXPECT_FALSE(replay.has_value());
    EXPECT_EQ(rig.store.last_device_id, "{0.0.1}.{aa}");
    EXPECT_EQ(rig.store.last_device_name, "Microphone Array (Cirrus Logic)");
    controller.Stop();
    EXPECT_EQ(rig.store.Calls().back(), "finalise s1");

    ASSERT_TRUE(controller.Start(ReplaySpec{"C:/tracks/elbow.wav", 4.0}, {}, true,
                                 {"{0.0.1}.{aa}", "Microphone Array"}));
    controller.Stop();
    ASSERT_TRUE(replay.has_value());
    EXPECT_EQ(replay->path, "C:/tracks/elbow.wav");
    EXPECT_EQ(replay->speed, 4.0);
    EXPECT_EQ(rig.store.last_device_id, "") << "a replay carries no device snapshot";
    EXPECT_EQ(rig.store.last_device_name, "");
}

TEST(SessionController, RetainReachesTheStoreAndLeavingSweeps) {
    Rig rig;
    auto controller = rig.Make(Script::kStreamUntilStopped);

    ASSERT_TRUE(controller.Start(std::nullopt, {}, false));
    EXPECT_FALSE(rig.store.last_retain);
    EXPECT_EQ(rig.store.sweeps, 1) << "the previous consultation is left at start";
    controller.Stop();

    controller.Close();
    EXPECT_EQ(rig.store.sweeps, 2) << "and at close";

    ASSERT_TRUE(controller.Start());
    EXPECT_TRUE(rig.store.last_retain) << "the default keeps";
    EXPECT_EQ(rig.store.sweeps, 3);
    controller.Stop();
}

TEST(SessionController,
     StopHandsTheCapturedAudioToDiarisationAndStoresOnlyTheAttributedTranscript) {
    Rig rig;
    rig.diariser.clusters = 2;
    rig.diariser.similarities = {0.2, 0.8};
    rig.diariser.speculate_first_turn = true;
    auto controller = rig.Make(Script::kStreamUntilStopped);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
    controller.Stop();

    const auto captured = rig.store.frames.size();
    EXPECT_EQ(rig.diariser.settles, 1) << "finalise decodes what capture had not reached";
    EXPECT_EQ(rig.diariser.settled_frames, captured);
    EXPECT_EQ(rig.diariser.calls, 1);
    EXPECT_EQ(rig.diariser.audio_frames, captured) << "the diariser hears exactly the capture";
    EXPECT_EQ(rig.events.progress, (std::vector<std::string>{"transcript", "speakers", "turns"}));
    EXPECT_EQ(rig.store.Calls(),
              (std::vector<std::string>{"begin s1", "replace s1", "finalise s1"}))
        << "the attributed transcript is the only one the store sees, before the seal";

    ASSERT_EQ(rig.store.turns.size(), 2u) << "one merged turn per cluster";
    const auto half = rig.diariser.audio_frames / 2;
    EXPECT_EQ(rig.diariser.takes, 1);
    EXPECT_EQ(rig.store.turns[0].text, "Speculated words.") << "the cache hit stands, tidied";
    EXPECT_EQ(rig.store.turns[1].first_frame, half);
    EXPECT_EQ(rig.store.turns[1].text,
              "Scripted turn 0, " + std::to_string(captured - half) + " frames.")
        << "the miss decodes fresh from its own audio";
    EXPECT_EQ(rig.store.turns[0].speaker, "patient");
    EXPECT_EQ(rig.store.turns[1].speaker, "doctor") << "the nearer cluster to the anchor";

    EXPECT_EQ(rig.diariser.similarity_calls, 1) << "the anchor is consulted once";
    EXPECT_EQ(rig.diariser.accruals.load(), 1) << "without a note lane the named session teaches";
    EXPECT_EQ(rig.diariser.accrued_cluster, 1);
}

TEST(SessionController, ThePrintLearnsNothingFromUnnamedSpeakers) {
    struct Row {
        const char* name;
        int clusters;
        std::vector<std::string> speakers;
    };
    const std::vector<Row> rows = {
        {"one cluster cannot be named", 1, {"speaker 1"}},
        // The scripted text carries no lexical role signal either
        {"two clusters and no anchor", 2, {"speaker 1", "speaker 2"}},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        Rig rig;
        rig.diariser.clusters = row.clusters;
        auto controller = rig.Make(Script::kStreamUntilStopped);

        ASSERT_TRUE(controller.Start());
        ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
        controller.Stop();

        std::vector<std::string> speakers;
        for (const auto& turn : rig.store.turns) speakers.push_back(turn.speaker);
        std::sort(speakers.begin(), speakers.end());  // numbering follows talk time
        EXPECT_EQ(speakers, row.speakers);
        EXPECT_EQ(rig.diariser.accruals.load(), 0);
    }
}

TEST(SessionController, CaptureTicksAdvanceDiarisationApplyCutsAndPrefillTheNote) {
    Rig rig;
    rig.transcriber.clip_cuts = {800, 2400};
    rig.diariser.speculate_transcript = true;
    // One 100 ms window per tick, so the first tick comes almost at once
    auto controller = rig.Make(Script::kStreamUntilStopped,
                               {.advance_frames = LevelMeter::kWindowFrames, .writer = true});

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(WaitFor([&] { return rig.diariser.advances.load() >= 2; }))
        << "the tick that produced cuts advances again at once, so a stop never waits on them";
    EXPECT_GE(rig.writer.prepares.load(), 1) << "the weights warm while the session records";
    controller.Stop();

    EXPECT_GT(rig.diariser.advanced_frames, 0u);
    EXPECT_LE(rig.diariser.advanced_frames, rig.store.frames.size())
        << "Advance only ever sees captured audio";
    EXPECT_EQ(rig.diariser.cut_points, (std::vector<std::uint64_t>{800, 2400}))
        << "the cuts reach the diariser once";
    EXPECT_GT(rig.writer.prefills.load(), 0) << "each tick hands the guess to the note lane";
    EXPECT_EQ(rig.writer.last_prefill_speaker, "doctor");
    EXPECT_EQ(rig.diariser.calls, 1) << "the full diarisation runs once, at finalise";
}

TEST(SessionController, FinaliseStagesAndDiariseTimingReachTheMetrics) {
    Rig rig;
    rig.diariser.timing.embed_s = 0.25;
    rig.diariser.timing.embed_misses = 3;
    auto controller = rig.Make(Script::kStreamUntilStopped, {.metrics = true});

    ASSERT_TRUE(controller.Start(ReplaySpec{"x.wav", 4.0}));
    ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
    controller.Stop();

    // The perf report reads these names
    const auto s = rig.registry.Take();
    EXPECT_TRUE(s.replay);
    EXPECT_EQ(s.replay_speed, 4.0);
    for (const char* stage : {"capture joined", "capture settled", "diarised", "voiceprints joined",
                              "diarise voiceprints"}) {
        EXPECT_TRUE(s.stage_seconds.contains(stage)) << stage;
    }
    EXPECT_EQ(s.stage_seconds.at("diarise embed"), 0.25);
    EXPECT_EQ(s.stage_seconds.at("diarise embed misses"), 3);
}

// Every decode leaves a chunk edge behind, as whisper's worker does
struct CuttingTranscriber : FakeTranscriber {
    std::vector<asr::Turn> DecodeClipChunks(std::span<const float> frames,
                                            std::uint64_t first_frame) override {
        clip_cuts.push_back(first_frame + frames.size() / 2);
        return FakeTranscriber::DecodeClipChunks(frames, first_frame);
    }
};

TEST(SessionController, NothingOneFinaliseLeavesBehindReachesTheNextSession) {
    Rig rig;
    CuttingTranscriber transcriber;
    SessionController controller(FactoryFor(Script::kStreamUntilStopped), rig.events, rig.store,
                                 transcriber, rig.vad, rig.diariser, kTestSettle,
                                 LevelMeter::kWindowFrames, nullptr, &rig.registry);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
    ASSERT_TRUE(WaitFor([&] { return rig.diariser.advances.load() >= 1; }));
    controller.Stop();
    ASSERT_FALSE(rig.store.turns.empty()) << "the finalise decoded its turns, leaving edges";

    ImportLog log;
    ASSERT_EQ(
        controller.Import(Reads(std::vector<float>(kTwoTurnFrames, 0.1F)), "", true, log.Report()),
        "s2");
    ASSERT_TRUE(log.Wait());
    EXPECT_TRUE(rig.diariser.cut_points.empty())
        << "the last finalise's chunk edges are not cut points in this recording";
    EXPECT_EQ(rig.registry.Take().diar_ticks, 0) << "capture ticks count per session";
}

}  // namespace
}  // namespace clinicavt::session
