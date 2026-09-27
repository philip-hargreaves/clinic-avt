#include "adapters/transcription/whisper_transcriber.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <openvino/genai/whisper_pipeline.hpp>
#include <utility>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/system/gpu_lease.hpp"
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
                           const std::string& device_override, metrics::Registry* metrics) {
    const models::ModelInfo& info = store.Resolve("asr", "default");
    const std::string requested = device_override.empty() ? info.device : device_override;
    // Only GPU work takes turns. On the NPU Whisper runs beside the note model
    const bool on_gpu = requested.rfind("GPU", 0) == 0;
    const auto take_gpu = [on_gpu](const char* who) {
        return on_gpu ? system::GpuLease::Global().Acquire(system::WatchForStuckHosts(who))
                      : system::GpuLease::Guard{};
    };
    // Taken before the seconds of device discovery, so Whisper is ready ahead
    // of a note model the shell asks for on connect
    std::shared_ptr<ov::genai::WhisperPipeline> pipeline;
    std::string device;
    {
        const auto lease = take_gpu("asr load");
        store.Verify(info);
        device = runtime.ResolveDevice(requested);
        std::fprintf(stderr, "clinicavt-engine: asr on %s\n", device.c_str());
        if (metrics != nullptr) metrics->RecordDevice("asr", device);
        pipeline = std::make_shared<ov::genai::WhisperPipeline>(info.dir, device,
                                                                models::CompileProperties(info));
    }
    auto config = pipeline->get_generation_config();
    config.language = "<|en|>";
    config.task = "transcribe";
    config.return_timestamps = true;

    // Transcript-tail conditioning (initial_prompt) was measured here and
    // rejected: it worsened WER even with register effects folded out
    return [pipeline, config, take_gpu](std::span<const float> frames, std::uint64_t first_frame) {
        const ov::genai::RawSpeechInput audio(frames.begin(), frames.end());
        // A stuck holder leaves the lease wedged, and the decode runs beside it
        const auto lease = take_gpu("asr");
        if (lease.waited() > 0.25) {
            std::fprintf(stderr, "clinicavt-engine: asr waited %.2f s for the GPU lease\n",
                         lease.waited());
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
                                       std::string device_override, metrics::Registry* metrics)
    : WhisperTranscriber(DeviceLoader([&store, &runtime, metrics](const std::string& device) {
                             return MakeWhisperDecode(store, runtime, device, metrics);
                         }),
                         std::move(device_override), metrics) {}

WhisperTranscriber::WhisperTranscriber(DeviceLoader loader, std::string device,
                                       metrics::Registry* metrics)
    : loader_([loader, device] { return loader(device); }),
      by_device_(std::move(loader)),
      metrics_(metrics) {
    worker_ = std::thread([this] { WorkerLoop(); });
}

bool WhisperTranscriber::SwitchDevice(std::string device,
                                      std::function<void(const std::string&)> done) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!by_device_ || stopping_) return false;
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
    worker_ = std::thread([this] { WorkerLoop(); });
}

WhisperTranscriber::~WhisperTranscriber() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    worker_.join();
}

std::vector<std::uint64_t> WhisperTranscriber::TakeClipCuts() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(clip_cuts_, {});
}

std::vector<Turn> WhisperTranscriber::DecodeClipChunks(std::span<const float> frames,
                                                       std::uint64_t first_frame) {
    std::future<std::vector<Turn>> chunks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return {};  // the worker no longer serves clips
        clips_.push_back({{frames.begin(), frames.end()}, first_frame, {}});
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

// Load off the hot path. A failed load drains clips without turns, so
// nothing hangs. Returns the error, empty on success
std::string WhisperTranscriber::LoadIfPending() {
    DecodeLoader loader;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loader = std::exchange(loader_, {});
    }
    if (!loader) {
        return {};
    }
    // The old device's model goes first, so two are never resident at once
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
        std::fprintf(stderr, "clinicavt-engine: transcription unavailable (%s)\n", e.what());
        return e.what();
    }
}

void WhisperTranscriber::WorkerLoop() {
    LoadIfPending();

    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        cv_.wait(lock, [this] { return !clips_.empty() || stopping_ || switching_; });
        if (stopping_) break;
        if (loader_) {
            auto done = std::exchange(switched_, {});
            switching_ = false;
            lock.unlock();
            const std::string error = LoadIfPending();
            moving_ = false;
            if (done) done(error);
            lock.lock();
            if (clips_.empty()) continue;
        }

        Clip clip = std::move(clips_.front());
        clips_.pop_front();
        lock.unlock();
        std::vector<Turn> chunks;
        std::vector<std::uint64_t> cuts;
        // A failed decode loses only this clip's text. The audio is
        // already stored
        try {
            if (decode_) {
                const auto t0 = std::chrono::steady_clock::now();
                const std::uint64_t clip_end = clip.first_frame + clip.frames.size();
                for (const Turn& turn : decode_(clip.frames, clip.first_frame)) {
                    if (turn.text.empty()) continue;
                    chunks.push_back(turn);
                    // Chunk edges: where a short answer inside a long clip
                    // begins and ends
                    for (const std::uint64_t edge :
                         {turn.first_frame, turn.first_frame + turn.frame_count}) {
                        if (edge > clip.first_frame && edge < clip_end) cuts.push_back(edge);
                    }
                }
                RecordDecode(clip.frames.size(), t0);
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
        // The cuts land before the caller is released, so a TakeClipCuts
        // right after the decode sees them
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
