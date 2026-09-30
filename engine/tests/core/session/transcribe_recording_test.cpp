#include "core/session/transcribe_recording.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "session_test_harness.hpp"

namespace clinicavt::session {
namespace {

using namespace test_harness;

// Two seconds, which FakeDiariser splits into two clusters at the midpoint
constexpr std::size_t kFrames = 2 * kSampleRate;

struct Recorded {
    std::vector<std::string> stages;
    StageFn Fn() {
        return [this](const char* name) { stages.emplace_back(name); };
    }
};

// Nothing captured ahead, so Capture and Voiceprints return null
struct CapabilityFreeDiariser : FakeDiariser {
    diar::ICaptureDiarisation* Capture() override {
        return nullptr;
    }
    diar::IVoiceprints* Voiceprints() override {
        return nullptr;
    }
};

TEST(TranscribeRecording, NamesTheNearerClusterTheDoctorAndDecodesEachTurn) {
    FakeDiariser diariser;
    diariser.clusters = 2;
    diariser.similarities = {0.2, 0.8};
    FakeTranscriber transcriber;
    RecordingEvents events;
    metrics::Registry metrics;
    Recorded recorded;
    const std::vector<float> audio(kFrames, 0.1f);

    const Transcript transcript =
        TranscribeRecording(audio, diariser, transcriber, events, &metrics, recorded.Fn());

    EXPECT_EQ(transcript.doctor_cluster, 1);
    EXPECT_EQ(transcript.diarised.cluster_count, 2);
    ASSERT_EQ(transcript.turns.size(), 2u);
    EXPECT_EQ(transcript.turns[0].speaker, "patient");
    EXPECT_EQ(transcript.turns[1].speaker, "doctor");
    EXPECT_EQ(transcript.turns[0].text,
              "Scripted turn 0, " + std::to_string(kFrames / 2) + " frames.");
    EXPECT_EQ(transcriber.decodes, 2) << "each turn from its own audio";
    EXPECT_EQ(diariser.similarity_calls, 1);
    EXPECT_EQ(diariser.takes, 1) << "the capture cache is read once";
    EXPECT_EQ(events.progress, (std::vector<std::string>{"speakers", "turns"}));
    EXPECT_EQ(recorded.stages, (std::vector<std::string>{"diarised", "turns decoded",
                                                         "voiceprints joined", "re-split"}));
    const auto taken = metrics.Take();
    EXPECT_EQ(taken.clusters, 2);
    EXPECT_TRUE(taken.stage_seconds.contains("diarise voiceprints"));
}

TEST(TranscribeRecording, ASpeculatedTurnIsTakenFromTheCaptureCache) {
    FakeDiariser diariser;
    diariser.clusters = 2;
    diariser.speculate_first_turn = true;
    FakeTranscriber transcriber;
    RecordingEvents events;
    Recorded recorded;
    const std::vector<float> audio(kFrames, 0.1f);

    const Transcript transcript =
        TranscribeRecording(audio, diariser, transcriber, events, nullptr, recorded.Fn());

    ASSERT_EQ(transcript.turns.size(), 2u);
    EXPECT_EQ(transcript.turns[0].text, "Speculated words.");
    EXPECT_EQ(transcriber.decodes, 1) << "only the miss is decoded";
    EXPECT_EQ(transcript.doctor_cluster, -1) << "no anchor and no lexical signal";
}

TEST(TranscribeRecording, ADiariserWithoutCapabilitiesStillTranscribes) {
    CapabilityFreeDiariser diariser;
    diariser.clusters = 2;
    diariser.speculate_first_turn = true;
    FakeTranscriber transcriber;
    RecordingEvents events;
    Recorded recorded;
    const std::vector<float> audio(kFrames, 0.1f);

    const Transcript transcript =
        TranscribeRecording(audio, diariser, transcriber, events, nullptr, recorded.Fn());

    ASSERT_EQ(transcript.turns.size(), 2u);
    EXPECT_EQ(transcriber.decodes, 2) << "no capture cache to read";
    EXPECT_EQ(diariser.takes, 0);
    EXPECT_EQ(recorded.stages.back(), "re-split");
}

TEST(TranscribeRecording, NothingHeardGivesNoTurns) {
    FakeDiariser diariser;
    FakeTranscriber transcriber;
    RecordingEvents events;
    Recorded recorded;

    const Transcript transcript =
        TranscribeRecording({}, diariser, transcriber, events, nullptr, recorded.Fn());

    EXPECT_TRUE(transcript.turns.empty());
    EXPECT_EQ(transcript.doctor_cluster, -1);
}

}  // namespace
}  // namespace clinicavt::session
