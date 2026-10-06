#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "core/common/worker_thread.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::models {
class ModelStore;
class OvRuntime;
}  // namespace clinicavt::models

namespace clinicavt::metrics {
class Registry;
}  // namespace clinicavt::metrics

namespace clinicavt::system {
class GpuLease;
}  // namespace clinicavt::system

namespace clinicavt::asr {

// Whisper chunks with absolute frames, which are also the source of cut points. Returns no chunks
// when `stop` is true before the decode starts
using DecodeFn =
    std::function<std::vector<Turn>(std::span<const float>, std::uint64_t, const StopFn& stop)>;

using DecodeLoader = std::function<DecodeFn()>;

using DeviceLoader = std::function<DecodeFn(const std::string& device)>;

// One worker thread loads, then decodes clips in order, so loading never blocks a caller
class WhisperTranscriber : public ITranscriber {
   public:
    // A non-empty device_override replaces the manifest's device for every decode
    WhisperTranscriber(const models::ModelStore& store, models::OvRuntime& runtime,
                       system::GpuLease& gpu, std::string device_override = "",
                       metrics::Registry* metrics = nullptr);
    explicit WhisperTranscriber(DecodeLoader loader,
                                metrics::Registry* metrics = nullptr);  // Tests inject the decode
    WhisperTranscriber(DeviceLoader loader, std::string device,
                       metrics::Registry* metrics = nullptr);  // Tests watch the device
    ~WhisperTranscriber() override;

    // Reloads the model on `device`, releasing the old one first, so the rest of the engine keeps
    // running. Queued clips decode on the new device. `done` runs on the worker with the error, or
    // an empty string, when the load ends. False if this transcriber cannot switch
    bool SwitchDevice(std::string device, std::function<void(const std::string&)> done);

    // True from a switch request until its load ends
    bool Moving() const {
        return moving_.load();
    }

    // Blocks until the worker has decoded or dropped the clip
    std::vector<Turn> DecodeClipChunks(std::span<const float> frames, std::uint64_t first_frame,
                                       const StopFn& stop) override;

    std::vector<std::uint64_t> TakeClipCuts() override;

   private:
    struct Clip {
        std::vector<float> frames;
        std::uint64_t first_frame;
        StopFn stop;
        std::promise<std::vector<Turn>> chunks;
    };

    void WorkerLoop();
    std::string Load(DecodeLoader loader);
    bool FallBackToGpu(const std::string& error);
    void RecordDecode(std::size_t frames, std::chrono::steady_clock::time_point t0);

    DecodeLoader loader_;
    DeviceLoader by_device_;  // set when the transcriber can move between devices
    // Under mutex_. Runs when the pending switch's load ends
    std::function<void(const std::string&)> switched_;
    bool switching_ = false;     // under mutex_
    std::string switch_device_;  // under mutex_, the device a pending switch loads
    std::string device_;         // worker thread once started, empty for the manifest's device
    std::atomic<bool> moving_{false};
    DecodeFn decode_;  // worker thread only once loaded
    metrics::Registry* metrics_ = nullptr;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Clip> clips_;
    std::vector<std::uint64_t> clip_cuts_;  // segment edges inside decoded clips
    WorkerThread worker_{mutex_, cv_};
};

}  // namespace clinicavt::asr
