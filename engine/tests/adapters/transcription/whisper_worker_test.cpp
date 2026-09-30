#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "adapters/transcription/whisper_transcriber.hpp"
#include "core/metrics/metrics.hpp"

namespace clinicavt::asr {
namespace {

Turn At(std::uint64_t first, std::uint64_t count, std::string text) {
    Turn turn;
    turn.first_frame = first;
    turn.frame_count = count;
    turn.text = std::move(text);
    return turn;
}

// A transcriber whose load hands back `decode` at once
DecodeLoader Ready(DecodeFn decode) {
    return [decode] { return decode; };
}

// Whisper's chunk edges inside a clip are where a short answer begins and
// ends, so they become cut points. The clip's own edges are not
TEST(WhisperWorker, DecodeClipJoinsChunkTextsAndInteriorEdgesBecomeCuts) {
    // A 3 s clip at frame 16000: chunks of 1.2 s and 1.75 s, then an empty one
    WhisperTranscriber transcriber(
        Ready([](std::span<const float>, std::uint64_t first, const StopFn&) {
            return std::vector<Turn>{At(first, 19200, "have you had any clots"),
                                     At(first + 19200, 28000, "not that I know of"),
                                     At(first + 47200, 800, "")};
        }));
    const std::vector<float> clip(48000, 0.1f);

    const auto chunks = transcriber.DecodeClipChunks(clip, 16000, {});
    ASSERT_EQ(chunks.size(), 2u) << "empty chunks are dropped";
    EXPECT_EQ(chunks[1].text, "not that I know of");
    EXPECT_EQ(transcriber.TakeClipCuts(), (std::vector<std::uint64_t>{35200, 35200, 63200}))
        << "the interior edge from both sides, and a chunk end short of the clip end";
    EXPECT_TRUE(transcriber.TakeClipCuts().empty()) << "taking drains";

    EXPECT_EQ(JoinedText(transcriber.DecodeClipChunks(clip, 16000, {})),
              "have you had any clots not that I know of");
}

TEST(WhisperWorker, AClipWaitsForTheOneLoadAndItsDecodeIsMetered) {
    metrics::Registry registry;
    std::atomic<int> loads{0};
    const DecodeLoader slow_load = [&loads] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ++loads;
        return DecodeFn([](std::span<const float>, std::uint64_t first, const StopFn&) {
            return std::vector<Turn>{At(first, 1, "w" + std::to_string(first))};
        });
    };
    {
        WhisperTranscriber transcriber(slow_load, &registry);
        const std::vector<float> clip(16000);
        EXPECT_EQ(JoinedText(transcriber.DecodeClipChunks(clip, 40, {})), "w40")
            << "waits for the load";
        EXPECT_EQ(JoinedText(transcriber.DecodeClipChunks(clip, 16040, {})), "w16040");
    }

    EXPECT_EQ(loads.load(), 1) << "one load serves every clip";
    const auto s = registry.Take();
    EXPECT_EQ(s.decoded_audio_seconds, 2.0);
    EXPECT_GE(s.decode_busy_seconds, 0.0);
}

// The old model is released before the new one loads, so two are never resident. A failed load
// must not leave later clips waiting
TEST(WhisperWorker, SwitchingDeviceLoadsThereAtOnceAndAFailedSwitchSaysWhy) {
    struct Load {
        std::string device;
        bool previous_released;
    };
    std::mutex mutex;
    std::vector<Load> loads;
    std::weak_ptr<std::string> resident;
    WhisperTranscriber transcriber(
        DeviceLoader([&](const std::string& device) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                loads.push_back({device, resident.expired()});
            }
            if (device == "BROKEN") throw std::runtime_error("no BROKEN device");
            auto model = std::make_shared<std::string>(device);
            resident = model;
            return DecodeFn([model](std::span<const float>, std::uint64_t first, const StopFn&) {
                return std::vector<Turn>{At(first, 1, *model)};
            });
        }),
        "GPU");
    const std::vector<float> clip(10);
    EXPECT_EQ(JoinedText(transcriber.DecodeClipChunks(clip, 0, {})), "GPU");

    const auto switch_to = [&transcriber](const std::string& device) {
        auto settled = std::make_shared<std::promise<std::string>>();
        auto done = settled->get_future();
        EXPECT_TRUE(transcriber.SwitchDevice(
            device, [settled](const std::string& error) { settled->set_value(error); }));
        // Settles at once, before any clip asks
        if (done.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            return std::string("not settled");
        }
        return done.get();
    };

    EXPECT_EQ(switch_to("NPU"), "");
    EXPECT_EQ(JoinedText(transcriber.DecodeClipChunks(clip, 10, {})), "NPU");

    EXPECT_EQ(switch_to("BROKEN"), "no BROKEN device");
    EXPECT_EQ(JoinedText(transcriber.DecodeClipChunks(clip, 20, {})), "")
        << "drained without text, not hung";

    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(loads.size(), 3u);
    EXPECT_EQ(loads[1].device, "NPU");
    EXPECT_EQ(loads[2].device, "BROKEN");
    for (const auto& load : loads) {
        EXPECT_TRUE(load.previous_released)
            << "a model was still resident when " << load.device << " loaded";
    }
}

// A switch asked for during another's load keeps the transcriber moving until it too has
// loaded, so the engine never counts the gap between the two as idle
TEST(WhisperWorker, ASwitchQueuedDuringALoadKeepsItMovingUntilBothSettle) {
    std::promise<void> npu_loading;
    std::promise<void> npu_gate;
    std::promise<void> cpu_gate;
    auto npu_released = npu_gate.get_future().share();
    auto cpu_released = cpu_gate.get_future().share();
    WhisperTranscriber transcriber(
        DeviceLoader([&](const std::string& device) {
            if (device == "NPU") {
                npu_loading.set_value();
                npu_released.wait();
            }
            if (device == "CPU") cpu_released.wait();
            return DecodeFn([](std::span<const float>, std::uint64_t, const StopFn&) {
                return std::vector<Turn>{};
            });
        }),
        "GPU");
    std::promise<std::string> npu_settled;
    std::promise<std::string> cpu_settled;
    auto first = npu_settled.get_future();
    auto second = cpu_settled.get_future();
    ASSERT_TRUE(transcriber.SwitchDevice(
        "NPU", [&](const std::string& error) { npu_settled.set_value(error); }));
    npu_loading.get_future().wait();
    ASSERT_TRUE(transcriber.SwitchDevice(
        "CPU", [&](const std::string& error) { cpu_settled.set_value(error); }));

    npu_gate.set_value();
    ASSERT_EQ(first.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(transcriber.Moving()) << "the queued switch has not loaded yet";

    cpu_gate.set_value();
    ASSERT_EQ(second.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(second.get(), "");
    EXPECT_FALSE(transcriber.Moving());
}

// A driver fault in one decode loses that clip's text, not the session
TEST(WhisperWorker, AThrowingDecodeLosesOnlyThatClipsText) {
    WhisperTranscriber transcriber(
        Ready([](std::span<const float>, std::uint64_t first, const StopFn&) {
            if (first == 0) throw std::runtime_error("driver");
            return std::vector<Turn>{At(first, 1, "later")};
        }));

    const std::vector<float> clip(10);
    EXPECT_EQ(JoinedText(transcriber.DecodeClipChunks(clip, 0, {})), "");
    EXPECT_TRUE(transcriber.TakeClipCuts().empty());
    EXPECT_EQ(JoinedText(transcriber.DecodeClipChunks(clip, 10, {})), "later");
}

// Stop drops a clip still waiting for the GPU and every clip queued behind it. Later clips decode
TEST(WhisperWorker, StoppedClipsAreDroppedWhileTheyWait) {
    std::atomic<int> decodes{0};
    WhisperTranscriber transcriber(
        Ready([&decodes](std::span<const float>, std::uint64_t first, const StopFn& stop) {
            ++decodes;
            if (!stop) return std::vector<Turn>{At(first, 1, "w" + std::to_string(first))};
            // Waits as the GPU lease does
            while (!stop()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return std::vector<Turn>{};
        }));
    const std::vector<float> clip(1600, 0.1f);
    std::atomic<bool> stopped{false};
    const StopFn stop = [&stopped] { return stopped.load(); };

    auto waiting =
        std::async(std::launch::async, [&] { return transcriber.DecodeClipChunks(clip, 0, stop); });
    while (decodes.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto queued = std::async(std::launch::async,
                             [&] { return transcriber.DecodeClipChunks(clip, 10, stop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stopped = true;

    ASSERT_EQ(waiting.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(queued.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(waiting.get().empty());
    EXPECT_TRUE(queued.get().empty());
    EXPECT_EQ(decodes.load(), 1) << "the queued clip never reached the model";
    EXPECT_EQ(JoinedText(transcriber.DecodeClipChunks(clip, 20, {})), "w20");
}

}  // namespace
}  // namespace clinicavt::asr
