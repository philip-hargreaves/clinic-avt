#include "adapters/vad/deferred_vad.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <thread>

namespace clinicavt {
namespace {

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

}  // namespace
}  // namespace clinicavt
