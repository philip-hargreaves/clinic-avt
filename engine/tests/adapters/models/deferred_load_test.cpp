#include "adapters/models/deferred_load.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "adapters/diarisation/deferred_diariser.hpp"
#include "adapters/vad/deferred_vad.hpp"

namespace clinicavt {
namespace {

TEST(DeferredLoad, GetWaitsForTheBuildAndRethrowsItsFailure) {
    std::atomic<bool> built{false};
    models::DeferredLoad<int> deferred("test", [&built] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        built = true;
        return std::make_unique<int>(7);
    });
    EXPECT_EQ(deferred.Get(), 7);
    EXPECT_TRUE(built.load());
    EXPECT_TRUE(deferred.Loaded());

    models::DeferredLoad<int> failed(
        "test", []() -> std::unique_ptr<int> { throw std::runtime_error("no model"); });
    EXPECT_THROW(failed.Get(), std::runtime_error);
    EXPECT_FALSE(failed.Loaded());
}

struct CountingVad : audio::IStreamingVad {
    std::atomic<int>& resets;

    explicit CountingVad(std::atomic<int>& counter) : resets(counter) {}

    float SpeechProbability(std::span<const float>) override {
        return 1.0f;
    }

    void Reset() override {
        ++resets;
    }
};

// The capture thread resets the VAD and must never block on its load
TEST(DeferredVad, NotReadyWhileLoadingAndResetNeverBlocks) {
    std::atomic<int> resets{0};
    audio::DeferredVad vad([&resets] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return std::make_unique<CountingVad>(resets);
    });

    EXPECT_FALSE(vad.Ready());
    vad.Reset();
    EXPECT_EQ(resets.load(), 0) << "a fresh model starts reset";

    (void)vad.SpeechProbability({});  // waits for the load
    EXPECT_TRUE(vad.Ready());
    vad.Reset();
    EXPECT_EQ(resets.load(), 1);
}

// Records every call that reaches it. The base class's defaults record nothing,
// so a method the wrapper does not forward is missing from the list
struct RecordingDiariser : diar::IDiariser {
    std::vector<std::string>& calls;

    explicit RecordingDiariser(std::vector<std::string>& log) : calls(log) {}

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
    void Advance(std::span<const float>, const diar::DecodeClipFn&) override {
        calls.push_back("Advance");
    }
    void Settle(std::span<const float> audio, const diar::DecodeClipFn&,
                const diar::StopFn&) override {
        calls.push_back("Settle " + std::to_string(audio.size()));
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

// The engine only ever sees the wrapper. A method it does not forward is a
// method the product does not have (one was missed once)
TEST(DeferredDiariser, ForwardsEveryMethodToTheLoadedDiariser) {
    std::vector<std::string> calls;
    diar::DeferredDiariser diariser(
        [&calls] { return std::make_unique<RecordingDiariser>(calls); });
    const std::vector<float> audio(320, 0.0f);
    const std::vector<std::uint64_t> cuts{16000, 32000};
    const diar::DecodeClipFn decode = [](std::span<const float>, std::uint64_t) {
        return std::vector<asr::Turn>{};
    };

    (void)diariser.Diarise(audio);
    EXPECT_EQ(diariser.AnchorSimilarities(audio, {}, 2), (std::vector<double>{0.75, 0.75}));
    diariser.Advance(audio, decode);
    diariser.Settle(audio, decode, [] { return false; });
    (void)diariser.TakeTurnTexts();
    (void)diariser.TakeTurnChunks();
    EXPECT_EQ(diariser.ClusterCentroids().size(), 2u);
    EXPECT_EQ(diariser.EmbedSpan(audio, 0, 320)[0], 320.0f);
    ASSERT_EQ(diariser.SpeculativeTranscript().size(), 1u);
    diariser.AddCutPoints(cuts);
    (void)diariser.EmbedVoice(audio);
    diariser.ReplaceAnchor(audio, 0);
    (void)diariser.DoctorVoiceprint(audio, {}, 0);
    diariser.AccrueVoiceprint(audio);
    diariser.DiscardCapture();

    EXPECT_EQ(calls, (std::vector<std::string>{
                         "Diarise", "AnchorSimilarities", "Advance", "Settle 320", "TakeTurnTexts",
                         "TakeTurnChunks", "ClusterCentroids", "EmbedSpan", "SpeculativeTranscript",
                         "AddCutPoints 2", "EmbedVoice", "ReplaceAnchor", "DoctorVoiceprint",
                         "AccrueVoiceprint", "DiscardCapture"}));
}

// Cancelling during the load must not wait for it: nothing has accumulated
TEST(DeferredDiariser, DiscardBeforeTheLoadIsANoOp) {
    std::vector<std::string> calls;
    std::atomic<bool> release{false};
    diar::DeferredDiariser diariser([&calls, &release] {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::make_unique<RecordingDiariser>(calls);
    });

    diariser.DiscardCapture();
    release = true;
    (void)diariser.Diarise({});
    EXPECT_EQ(calls, (std::vector<std::string>{"Diarise"})) << "the early discard reached nothing";

    diariser.DiscardCapture();
    EXPECT_EQ(calls.back(), "DiscardCapture");
}

}  // namespace
}  // namespace clinicavt
