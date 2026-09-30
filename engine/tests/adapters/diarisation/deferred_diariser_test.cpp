#include "adapters/diarisation/deferred_diariser.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace clinicavt {
namespace {

// Records every call that reaches it. With capabilities=false it has none, like the stand-ins
struct RecordingDiariser : diar::IDiariser, diar::ICaptureDiarisation, diar::IVoiceprints {
    std::vector<std::string>& calls;
    bool capabilities = true;

    explicit RecordingDiariser(std::vector<std::string>& log, bool with_capabilities = true)
        : calls(log), capabilities(with_capabilities) {}

    diar::DiariseResult Diarise(std::span<const float>) override {
        calls.push_back("Diarise");
        return {};
    }
    std::vector<double> AnchorSimilarities(std::span<const float>,
                                           const std::vector<diar::LabelledSlice>&,
                                           int cluster_count) override {
        calls.push_back("AnchorSimilarities");
        return std::vector<double>(static_cast<std::size_t>(cluster_count), 0.75);
    }
    diar::ICaptureDiarisation* Capture() override {
        return capabilities ? this : nullptr;
    }
    diar::IVoiceprints* Voiceprints() override {
        return capabilities ? this : nullptr;
    }
    void Advance(std::span<const float>, const diar::DecodeClipFn&) override {
        calls.push_back("Advance");
    }
    void Settle(std::span<const float> audio, const diar::DecodeClipFn&,
                const diar::StopFn&) override {
        calls.push_back("Settle " + std::to_string(audio.size()));
    }
    void FindSpeech(std::span<const float> audio, const std::function<void(double)>&,
                    const diar::StopFn&) override {
        calls.push_back("FindSpeech " + std::to_string(audio.size()));
    }
    diar::TurnTexts TakeTurnTexts() override {
        calls.push_back("TakeTurnTexts");
        return {};
    }
    diar::TurnChunks TakeTurnChunks() override {
        calls.push_back("TakeTurnChunks");
        return {};
    }
    std::vector<std::vector<float>> ClusterCentroids() override {
        calls.push_back("ClusterCentroids");
        return {{1.0f, 0.0f}, {0.0f, 1.0f}};
    }
    std::vector<float> EmbedSpan(std::span<const float>, std::uint64_t first,
                                 std::uint64_t end) override {
        calls.push_back("EmbedSpan");
        return {static_cast<float>(end - first), 0.0f};
    }
    std::vector<asr::Turn> SpeculativeTranscript() override {
        calls.push_back("SpeculativeTranscript");
        return {{0, 16000, "doctor", "guessed"}};
    }
    void AddCutPoints(std::span<const std::uint64_t> cuts) override {
        calls.push_back("AddCutPoints " + std::to_string(cuts.size()));
    }
    std::vector<float> EmbedVoice(std::span<const float>) override {
        calls.push_back("EmbedVoice");
        return {1.0f};
    }
    void ReplaceAnchor(std::span<const float>, std::uint64_t) override {
        calls.push_back("ReplaceAnchor");
    }
    std::vector<float> DoctorVoiceprint(std::span<const float>,
                                        const std::vector<diar::LabelledSlice>&, int) override {
        calls.push_back("DoctorVoiceprint");
        return {1.0f};
    }
    void AccrueVoiceprint(std::span<const float>) override {
        calls.push_back("AccrueVoiceprint");
    }
    void DiscardCapture() override {
        calls.push_back("DiscardCapture");
    }
};

const std::vector<std::string> kEveryCall = {
    "Diarise",        "AnchorSimilarities", "Advance",          "Settle 320",
    "FindSpeech 320", "TakeTurnTexts",      "TakeTurnChunks",   "AddCutPoints 2",
    "DiscardCapture", "ClusterCentroids",   "EmbedSpan",        "SpeculativeTranscript",
    "EmbedVoice",     "ReplaceAnchor",      "DoctorVoiceprint", "AccrueVoiceprint"};

// Calls every method of the port and its capabilities once, in kEveryCall's order
void CallEverything(diar::IDiariser& diariser) {
    const std::vector<float> audio(320, 0.0f);
    const std::vector<std::uint64_t> cuts{16000, 32000};
    const diar::DecodeClipFn decode = [](std::span<const float>, std::uint64_t) {
        return std::vector<asr::Turn>{};
    };
    (void)diariser.Diarise(audio);
    (void)diariser.AnchorSimilarities(audio, {}, 2);
    auto* const capture = diariser.Capture();
    auto* const voiceprints = diariser.Voiceprints();
    ASSERT_NE(capture, nullptr);
    ASSERT_NE(voiceprints, nullptr);
    capture->Advance(audio, decode);
    capture->Settle(audio, decode, [] { return false; });
    capture->FindSpeech(audio, {}, [] { return false; });
    (void)capture->TakeTurnTexts();
    (void)capture->TakeTurnChunks();
    capture->AddCutPoints(cuts);
    capture->DiscardCapture();
    (void)voiceprints->ClusterCentroids();
    (void)voiceprints->EmbedSpan(audio, 0, 320);
    (void)capture->SpeculativeTranscript();
    (void)voiceprints->EmbedVoice(audio);
    voiceprints->ReplaceAnchor(audio, 0);
    (void)voiceprints->DoctorVoiceprint(audio, {}, 0);
    voiceprints->AccrueVoiceprint(audio);
}

TEST(DeferredDiariser, TheRecorderSeesEveryCallMadeDirectly) {
    std::vector<std::string> calls;
    RecordingDiariser direct(calls);
    CallEverything(direct);
    EXPECT_EQ(calls, kEveryCall) << "the list the wrapper test compares against is complete";
}

// The engine only sees the wrapper, so a method it doesn't forward is effectively missing
TEST(DeferredDiariser, ForwardsEveryCapabilityToTheLoadedDiariser) {
    std::vector<std::string> calls;
    diar::DeferredDiariser diariser(
        [&calls] { return std::make_unique<RecordingDiariser>(calls); });
    CallEverything(diariser);
    EXPECT_EQ(calls, kEveryCall);

    const std::vector<float> audio(320, 0.0f);
    EXPECT_EQ(diariser.AnchorSimilarities(audio, {}, 2), (std::vector<double>{0.75, 0.75}));
    EXPECT_EQ(diariser.Voiceprints()->ClusterCentroids().size(), 2u);
    EXPECT_EQ(diariser.Voiceprints()->EmbedSpan(audio, 0, 320)[0], 320.0f);
    EXPECT_EQ(diariser.Capture()->SpeculativeTranscript().size(), 1u);
}

// A stand-in diariser has no capabilities, so each forwarded call does nothing
TEST(DeferredDiariser, ACapabilityTheLoadedDiariserLacksDoesNothing) {
    std::vector<std::string> calls;
    diar::DeferredDiariser diariser(
        [&calls] { return std::make_unique<RecordingDiariser>(calls, false); });
    CallEverything(diariser);
    EXPECT_EQ(calls, (std::vector<std::string>{"Diarise", "AnchorSimilarities"}));

    const std::vector<float> audio(320, 0.0f);
    EXPECT_TRUE(diariser.Capture()->TakeTurnTexts().empty());
    EXPECT_TRUE(diariser.Capture()->SpeculativeTranscript().empty());
    EXPECT_TRUE(diariser.Voiceprints()->EmbedVoice(audio).empty());
    EXPECT_TRUE(diariser.Voiceprints()->DoctorVoiceprint(audio, {}, 0).empty());
}

// Cancelling during the load must not wait for it, since nothing has accumulated
TEST(DeferredDiariser, DiscardBeforeTheLoadIsANoOp) {
    std::vector<std::string> calls;
    std::atomic<bool> release{false};
    diar::DeferredDiariser diariser([&calls, &release] {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::make_unique<RecordingDiariser>(calls);
    });

    diariser.Capture()->DiscardCapture();
    release = true;
    (void)diariser.Diarise({});
    EXPECT_EQ(calls, (std::vector<std::string>{"Diarise"})) << "the early discard reached nothing";

    diariser.Capture()->DiscardCapture();
    EXPECT_EQ(calls.back(), "DiscardCapture");
}

}  // namespace
}  // namespace clinicavt
