#include "adapters/transcription/whisper_transcriber.hpp"

#include <chrono>
#include <memory>
#include <openvino/genai/whisper_pipeline.hpp>
#include <utility>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/system/gpu_lease.hpp"
#include "core/common/log.hpp"
#include "core/metrics/metrics.hpp"
#include "ports/audio_source.hpp"

namespace clinicavt::asr {

namespace {

std::string Trimmed(const std::string& text) {
    const auto begin = text.find_first_not_of(' ');
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(' ') - begin + 1);
}

DecodeFn MakeWhisperDecode(const models::ModelStore& store, models::OvRuntime& runtime,
                           system::GpuLease& gpu, const std::string& device_override,
                           metrics::Registry* metrics) {
    const models::ModelInfo& info = store.Resolve("asr", "default");
    const std::string requested = device_override.empty() ? info.device : device_override;
    // Only GPU work takes the lease; NPU Whisper runs alongside the note model
    const bool on_gpu = requested.rfind("GPU", 0) == 0;
    const auto take_gpu = [on_gpu, &gpu](const char* who, const StopFn& stop = {}) {
        return on_gpu ? gpu.Acquire(system::WatchForStuckHosts(who, gpu), stop)
                      : system::GpuLease::Guard{};
    };
    // Taken before device discovery (seconds) so Whisper is ready before the note
    // model the shell requests on connect
    std::shared_ptr<ov::genai::WhisperPipeline> pipeline;
    std::string device;
    {
        const auto lease = take_gpu("asr load");
        store.Verify(info);
        device = runtime.ResolveDevice(requested);
        log::Printf("clinicavt-engine: asr on %s\n", device.c_str());
        if (metrics != nullptr) metrics->RecordDevice("asr", device);
        pipeline = std::make_shared<ov::genai::WhisperPipeline>(info.dir, device,
                                                                models::CompileProperties(info));
    }
    auto config = pipeline->get_generation_config();
    config.language = "<|en|>";
    config.task = "transcribe";
    config.return_timestamps = true;

    // initial_prompt (transcript-tail conditioning) was tested and rejected because it
    // worsened WER even with register effects folded out
    return [pipeline, config, take_gpu](std::span<const float> frames, std::uint64_t first_frame,
                                        const StopFn& stop) -> std::vector<Turn> {
        const ov::genai::RawSpeechInput audio(frames.begin(), frames.end());
        // If a stuck holder wedged the lease, decode anyway alongside it
        const auto lease = take_gpu("asr", stop);
        if (stop && stop()) return {};
        if (lease.Waited() > 0.25) {
            log::Printf("clinicavt-engine: asr waited %.2f s for the GPU lease\n", lease.Waited());
        }
        auto result = pipeline->generate(audio, config);

        std::vector<Turn> turns;
        if (result.chunks.has_value()) {
            const float clip_end = static_cast<float>(frames.size()) / audio::kSampleRate;
            for (const auto& chunk : *result.chunks) {
                Turn turn;
                // Stamps can overrun the clip (the window is padded to 30 s): clamp
                const float start = std::min(std::max(0.0f, chunk.start_ts), clip_end);
                turn.first_frame =
                    first_frame + static_cast<std::uint64_t>(start * audio::kSampleRate);
                // An open-ended last chunk reports end_ts -1
                const float end =
                    chunk.end_ts > chunk.start_ts ? std::min(chunk.end_ts, clip_end) : clip_end;
                turn.frame_count =
                    static_cast<std::uint64_t>((end - chunk.start_ts) * audio::kSampleRate);
                turn.text = Trimmed(chunk.text);
                if (!turn.text.empty()) turns.push_back(std::move(turn));
            }
        } else {
            Turn turn;
            turn.first_frame = first_frame;
            turn.frame_count = frames.size();
            turn.text = Trimmed(result);
            if (!turn.text.empty()) turns.push_back(std::move(turn));
        }
        return turns;
    };
}

}  // namespace

WhisperTranscriber::WhisperTranscriber(const models::ModelStore& store, models::OvRuntime& runtime,
                                       system::GpuLease& gpu, std::string device_override,
                                       metrics::Registry* metrics)
    : WhisperTranscriber(DeviceLoader([&store, &runtime, &gpu, metrics](const std::string& device) {
                             return MakeWhisperDecode(store, runtime, gpu, device, metrics);
                         }),
                         std::move(device_override), metrics) {}

WhisperTranscriber::WhisperTranscriber(DeviceLoader loader, std::string device,
                                       metrics::Registry* metrics)
    : loader_([loader, device = std::move(device)] { return loader(device); }),
      by_device_(std::move(loader)),
      metrics_(metrics) {
    worker_.Start([this] { WorkerLoop(); });
}

bool WhisperTranscriber::SwitchDevice(std::string device,
                                      std::function<void(const std::string&)> done) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!by_device_ || worker_.Stopping()) return false;
        loader_ = [loader = by_device_, device = std::move(device)] { return loader(device); };
        switched_ = std::move(done);
        switching_ = true;
        moving_ = true;
    }
    cv_.notify_all();
    return true;
}

WhisperTranscriber::WhisperTranscriber(DecodeLoader loader, metrics::Registry* metrics)
    : loader_(std::move(loader)), metrics_(metrics) {
    worker_.Start([this] { WorkerLoop(); });
}

WhisperTranscriber::~WhisperTranscriber() {
    worker_.Stop();
}

std::vector<std::uint64_t> WhisperTranscriber::TakeClipCuts() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(clip_cuts_, {});
}

std::vector<Turn> WhisperTranscriber::DecodeClipChunks(std::span<const float> frames,
                                                       std::uint64_t first_frame,
                                                       const StopFn& stop) {
    std::future<std::vector<Turn>> chunks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (worker_.Stopping()) return {};  // the worker no longer serves clips
        clips_.push_back({{frames.begin(), frames.end()}, first_frame, stop, {}});
        chunks = clips_.back().chunks.get_future();
    }
    cv_.notify_all();
    return chunks.get();
}

void WhisperTranscriber::RecordDecode(std::size_t frames,
                                      std::chrono::steady_clock::time_point t0) {
    if (metrics_ != nullptr) {
        metrics_->RecordDecode(
            static_cast<double>(frames) / audio::kSampleRate,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
}

// Loads off the hot path. On failure clips drain with no turns, so nothing hangs.
// Returns the error, empty on success
// NOLINTNEXTLINE(performance-unnecessary-value-param) owned, so freed once loaded
std::string WhisperTranscriber::Load(DecodeLoader loader) {
    if (!loader) {
        return {};
    }
    // Release the old device's model first so two are never resident
    decode_ = {};
    const auto t0 = std::chrono::steady_clock::now();
    try {
        decode_ = loader();
        if (metrics_ != nullptr) {
            metrics_->RecordLoad(
                "asr",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        return {};
    } catch (const std::exception& e) {
        log::Printf("clinicavt-engine: transcription unavailable (%s)\n", e.what());
        return e.what();
    }
}

void WhisperTranscriber::WorkerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    // A switch requested before the first load replaces it
    if (!switching_) {
        auto first = std::exchange(loader_, {});
        lock.unlock();
        Load(std::move(first));
        lock.lock();
    }
    while (!worker_.Stopping()) {
        cv_.wait(lock, [this] { return !clips_.empty() || worker_.Stopping() || switching_; });
        if (worker_.Stopping()) break;
        // Take a switch's load and reply together so a later switch cannot split them
        if (switching_) {
            auto loader = std::exchange(loader_, {});
            auto done = std::exchange(switched_, {});
            switching_ = false;
            lock.unlock();
            const std::string error = Load(std::move(loader));
            lock.lock();
            moving_ = switching_;  // a switch requested during this load is still pending
            lock.unlock();
            if (done) done(error);
            lock.lock();
            continue;
        }

        Clip clip = std::move(clips_.front());
        clips_.pop_front();
        lock.unlock();
        std::vector<Turn> chunks;
        std::vector<std::uint64_t> cuts;
        // A failed decode loses only this clip's text; the audio is already stored
        try {
            if (decode_ && !(clip.stop && clip.stop())) {
                const auto t0 = std::chrono::steady_clock::now();
                const std::uint64_t clip_end = clip.first_frame + clip.frames.size();
                for (const Turn& turn : decode_(clip.frames, clip.first_frame, clip.stop)) {
                    if (turn.text.empty()) continue;
                    chunks.push_back(turn);
                    // Chunk edges mark a short answer's start and end inside a long clip
                    for (const std::uint64_t edge :
                         {turn.first_frame, turn.first_frame + turn.frame_count}) {
                        if (edge > clip.first_frame && edge < clip_end) cuts.push_back(edge);
                    }
                }
                RecordDecode(clip.frames.size(), t0);
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch) this clip loses its text
        }
        // Record cuts before releasing the caller so TakeClipCuts right after sees them
        lock.lock();
        clip_cuts_.insert(clip_cuts_.end(), cuts.begin(), cuts.end());
        lock.unlock();
        clip.chunks.set_value(std::move(chunks));
        lock.lock();
    }
    // A caller may still be blocked on a pending clip at shutdown
    for (auto& clip : clips_) clip.chunks.set_value({});
}

}  // namespace clinicavt::asr
