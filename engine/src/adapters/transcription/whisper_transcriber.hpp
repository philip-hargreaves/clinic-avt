#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ports/transcriber.hpp"

namespace clinicavt::models {
class ModelStore;
class OvRuntime;
}  // namespace clinicavt::models

namespace clinicavt::metrics {
class Registry;
}  // namespace clinicavt::metrics

namespace clinicavt::asr {

// One decode: Whisper's chunks with absolute frames, the cut-point source
using DecodeFn = std::function<std::vector<Turn>(std::span<const float>, std::uint64_t)>;

using DecodeLoader = std::function<DecodeFn()>;

// Loads the model for one device, GPU or NPU
using DeviceLoader = std::function<DecodeFn(const std::string& device)>;

// Whisper behind the transcriber port: one worker thread loads then decodes
// clips in order, so the load never blocks a caller
class WhisperTranscriber : public ITranscriber {
   public:
    // device_override: the device every decode runs on, else the manifest's
    WhisperTranscriber(const models::ModelStore& store, models::OvRuntime& runtime,
                       std::string device_override = "", metrics::Registry* metrics = nullptr);
    explicit WhisperTranscriber(DecodeLoader loader,
                                metrics::Registry* metrics = nullptr);  // Tests inject the decode
    WhisperTranscriber(DeviceLoader loader, std::string device,
                       metrics::Registry* metrics = nullptr);  // Tests watch the device
    ~WhisperTranscriber() override;

    // Loads the same model again on `device`, releasing the old one first, so
    // nothing else in the engine restarts. Clips queued meanwhile decode on the
    // new device. `done` runs on the worker once the load settles, with the
    // error or an empty string. False when this transcriber cannot switch
    bool SwitchDevice(std::string device, std::function<void(const std::string&)> done);

    // Blocks until the worker has decoded the clip
    std::vector<Turn> DecodeClipChunks(std::span<const float> frames,
                                       std::uint64_t first_frame) override;

    std::vector<std::uint64_t> TakeClipCuts() override;

   private:
    struct Clip {
        std::vector<float> frames;
        std::uint64_t first_frame;
        std::promise<std::vector<Turn>> chunks;
    };

    void WorkerLoop();
    std::string LoadIfPending();
    void RecordDecode(std::size_t frames, std::chrono::steady_clock::time_point t0);

    DecodeLoader loader_;
    DeviceLoader by_device_;  // set when the transcriber can move between devices
    std::function<void(const std::string&)> switched_;  // under mutex_, the pending switch's
    bool switching_ = false;                            // under mutex_
    DecodeFn decode_;  // Worker-thread only once the loader has run
    metrics::Registry* metrics_ = nullptr;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Clip> clips_;
    std::vector<std::uint64_t> clip_cuts_;  // segment edges inside decoded clips
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace clinicavt::asr
