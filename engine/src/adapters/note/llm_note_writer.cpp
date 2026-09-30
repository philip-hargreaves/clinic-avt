#include "adapters/note/llm_note_writer.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/note/note_prompt.hpp"
#include "adapters/note/text_pipeline.hpp"
#include "adapters/system/awake_request.hpp"
#include "adapters/system/gpu_lease.hpp"
#include "core/common/log.hpp"
#include "core/note/model_failure.hpp"

namespace clinicavt::note {

namespace {

std::string Trimmed(const std::string& text) {
    const auto begin = text.find_first_not_of(" \n\r\t");
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(" \n\r\t") - begin + 1);
}

std::size_t SharedPrefix(const std::string& a, const std::string& b) {
    std::size_t n = 0;
    while (n < a.size() && n < b.size() && a[n] == b[n]) ++n;
    return n;
}

const char* StyleFile(const NoteOptions& options) {
    return options.style == NoteStyle::kSoap ? "note-soap.md" : "note-narrative.md";
}

double Seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

// Template the prompts were tuned with. The prefill must start with exactly
// these bytes for KV reuse
constexpr const char* kUserTurn = "<|im_start|>user\n";

ov::genai::GenerationConfig Greedy(std::size_t max_new_tokens) {
    ov::genai::GenerationConfig config;
    config.max_new_tokens = max_new_tokens;
    config.do_sample = false;
    config.apply_chat_template = false;
    return config;
}

}  // namespace

struct LlmNoteWriter::Impl {
    const models::ModelStore& store;
    models::OvRuntime& runtime;
    system::GpuLease& gpu;
    std::filesystem::path prompt_dir;
    std::string tier;
    std::mutex swap_mutex;      // guards pipeline
    std::mutex state_mutex;     // guards loader, load_error, loading, on_load
    std::mutex generate_mutex;  // one generate at a time, so a prefill never overlaps a note
    std::string last_prefill;   // under generate_mutex: last prompt the KV cache was extended to
    std::shared_ptr<TextPipeline> pipeline;
    std::exception_ptr load_error;
    std::thread loader;
    std::atomic<bool> loading{false};
    std::atomic<bool> cancel{false};
    std::atomic<bool> closed{false};
    LoadListener on_load;
    std::function<void(double)> on_gpu_wait;  // under state_mutex

    // Waits for the GPU lease, reporting liveness meanwhile. Throws if a stuck host
    // holds it, since the wait would never end
    system::GpuLease::Guard TakeGpu(const char* who) {
        auto watch = system::WatchForStuckHosts(who, gpu);
        auto guard = gpu.Acquire([this, &watch](double waited) {
            std::function<void(double)> listener;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                listener = on_gpu_wait;
            }
            if (listener) listener(waited);
            return watch(waited);
        });
        if (gpu.Active() && !guard.Held() && gpu.Wedged()) throw std::runtime_error(kStuckInDriver);
        return guard;
    }

    LoadReport Load() {
        const models::ModelInfo& info = store.Resolve("note", tier);
        LoadReport report;
        report.id = info.id;
        report.name = info.name;
        report.first_use = !models::Compiled(info);
        const auto t0 = std::chrono::steady_clock::now();
        store.Verify(info);
        const double verified = Seconds(t0);
        const std::string device = runtime.ResolveDevice(info.device);
        // Build and warm-up hold the GPU lease (nothing else runs) and a power request
        // (no standby mid-load)
        const system::AwakeRequest awake(L"ClinicAVT: loading the note model");
        const auto lease = TakeGpu("note load");
        std::shared_ptr<TextPipeline> built = MakeTextPipeline(info, device);
        report.seconds = Seconds(t0);
        log::Printf(
            "clinicavt-note-host: note %s (%s, %s) on %s, checked in %.1f s, loaded in %.1f s, "
            "lease wait %.2f s\n",
            info.id.c_str(), tier.c_str(), info.pipeline.c_str(), device.c_str(), verified,
            report.seconds, lease.Waited());
        WarmPromptPrefix(*built);
        std::lock_guard<std::mutex> lock(swap_mutex);
        pipeline = std::move(built);
        report.ok = true;
        return report;
    }

    // One discarded token caches the instruction block's KV, so stop prefills only the
    // transcript (measured 2.1 -> 1.3 s). Notes are equivalent but not byte-identical
    void WarmPromptPrefix(TextPipeline& built) {
        try {
            const auto t0 = std::chrono::steady_clock::now();
            built.Generate(kUserTurn + LoadPrompt(prompt_dir / "note-narrative.md"), Greedy(1),
                           nullptr);
            log::Printf("clinicavt-note-host: note prefix warmed in %.1f s\n", Seconds(t0));
        } catch (const std::exception& e) {
            if (PoisonsGpuContext(e.what())) throw;
            log::Printf("clinicavt-note-host: note prefix warm failed (%s)\n", e.what());
        }
    }

    std::shared_ptr<TextPipeline> Pipeline() {
        std::lock_guard<std::mutex> lock(swap_mutex);
        return pipeline;
    }

    void Report(const LoadReport& report) {
        LoadListener listener;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            listener = on_load;
        }
        if (listener) listener(report);
    }

    void JoinLoader() {
        std::thread finished;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            finished = std::move(loader);
        }
        if (finished.joinable()) {
            finished.join();
        }
    }
};

LlmNoteWriter::LlmNoteWriter(const models::ModelStore& store, models::OvRuntime& runtime,
                             system::GpuLease& gpu, std::filesystem::path prompt_dir,
                             std::string tier, LoadListener on_load,
                             std::function<void(double)> on_gpu_wait)
    : impl_(new Impl{store, runtime, gpu, std::move(prompt_dir), std::move(tier)}) {
    impl_->on_load = std::move(on_load);
    impl_->on_gpu_wait = std::move(on_gpu_wait);
}

LlmNoteWriter::~LlmNoteWriter() {
    impl_->JoinLoader();
}

// Starts the background load so the ~14 s cost falls during capture instead of
// at stop. A failed attempt is retried on the next call
void LlmNoteWriter::Prepare() {
    std::thread finished;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (impl_->loading.load() || impl_->Pipeline() != nullptr) {
            return;
        }
        finished = std::move(impl_->loader);
        impl_->load_error = nullptr;
        impl_->loading = true;
        impl_->loader = std::thread([impl = impl_.get()] {
            LoadReport report;
            try {
                report = impl->Load();
            } catch (const std::exception& e) {
                {
                    std::lock_guard<std::mutex> lock(impl->state_mutex);
                    impl->load_error = std::current_exception();
                }
                report.ok = false;
                report.detail = e.what();
                log::Printf("clinicavt-note-host: note load failed (%s)\n", e.what());
            }
            impl->loading = false;
            impl->Report(report);
        });
    }
    if (finished.joinable()) {
        finished.join();
    }
}

std::string LlmNoteWriter::Write(const std::vector<asr::Turn>& transcript,
                                 const NoteOptions& options, const Progress& progress) {
    if (transcript.empty()) {
        throw std::runtime_error("nothing to write: the transcript is empty");
    }
    // Confirmation goes after everything the capture-phase prefill covered
    return Generate(
        LoadPrompt(impl_->prompt_dir / StyleFile(options)) + TranscriptBlock(transcript) + "\n" +
            LoadPrompt(impl_->prompt_dir /
                       (std::string("detail-") + NoteDetailName(options.detail) + ".md")) +
            (options.confirmed ? LoadPrompt(impl_->prompt_dir / "confirmed.md") : ""),
        progress);
}

std::string LlmNoteWriter::WritePatient(const std::string& note, const Progress& progress) {
    if (note.empty()) {
        throw std::runtime_error("nothing to write: the note is empty");
    }
    return Generate(LoadPrompt(impl_->prompt_dir / "patient-info.md") + note + "\n", progress);
}

std::string LlmNoteWriter::WriteLabel(const std::string& note) {
    if (note.empty()) {
        return {};
    }
    return Generate(LoadPrompt(impl_->prompt_dir / "label.md") + note + "\n", nullptr, 16);
}

std::string LlmNoteWriter::WriteSummary(const std::string& note) {
    if (note.empty()) {
        throw std::runtime_error("nothing to summarise: the note is empty");
    }
    return Generate(LoadPrompt(impl_->prompt_dir / "case-summary.md") + note + "\n", nullptr, 160);
}

void LlmNoteWriter::Prefill(const std::vector<asr::Turn>& transcript, const NoteOptions& options) {
    if (transcript.empty()) return;
    const auto pipeline = impl_->Pipeline();
    if (pipeline == nullptr) return;
    std::unique_lock<std::mutex> lock(impl_->generate_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const std::string prompt = kUserTurn + LoadPrompt(impl_->prompt_dir / StyleFile(options)) +
                               TranscriptBlock(transcript);
    if (prompt == impl_->last_prefill) return;
    try {
        // No retry. If the GPU is busy, skip this prefill
        auto& gpu = impl_->gpu;
        const auto lease = gpu.TryAcquire();
        if (gpu.Active() && !lease.Held()) return;
        const auto t0 = std::chrono::steady_clock::now();
        const TextPipeline::Result result = pipeline->Generate(prompt, Greedy(1), nullptr);
        log::Printf(
            "clinicavt-note-host: prefill %zu turns, %zu tokens, %zu shared chars, %.2f s\n",
            transcript.size(), result.input_tokens, SharedPrefix(prompt, impl_->last_prefill),
            Seconds(t0));
        impl_->last_prefill = prompt;
    } catch (const std::exception& e) {
        impl_->last_prefill.clear();
        // Rethrown so the host decides whether to exit
        if (PoisonsGpuContext(e.what())) throw;
        log::Printf("clinicavt-note-host: prefill failed (%s)\n", e.what());
    }
}

std::string LlmNoteWriter::Generate(const std::string& prompt, const Progress& progress,
                                    std::size_t max_new_tokens) {
    impl_->cancel = impl_->closed.load();
    Prepare();
    impl_->JoinLoader();
    // Waits for any running prefill, then compares its prompt with this one
    std::lock_guard<std::mutex> generation(impl_->generate_mutex);
    // Hold a strong ref for the whole generation so a swap or teardown cannot free
    // the model mid-call
    const auto pipeline = impl_->Pipeline();
    if (pipeline == nullptr) {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (impl_->load_error != nullptr) {
            std::rethrow_exception(impl_->load_error);
        }
        throw std::runtime_error("note model unavailable");
    }

    const std::string wrapped =
        kUserTurn + prompt + "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";

    if (!impl_->last_prefill.empty()) {
        const std::size_t shared = SharedPrefix(wrapped, impl_->last_prefill);
        log::Printf("clinicavt-note-host: prompt %zu chars, prefill covered %zu (%.0f%%)\n",
                    wrapped.size(), shared, 100.0 * shared / wrapped.size());
        impl_->last_prefill.clear();
    }
    // The streamer sends partials out and receives cancel. On cancel the text so
    // far is returned
    std::string text;
    const TextPipeline::Streamer streamer = [this, &text, &progress](const std::string& piece) {
        if (impl_->cancel.load()) {
            return ov::genai::StreamingStatus::STOP;
        }
        text += piece;
        if (progress) {
            progress(text);
        }
        return ov::genai::StreamingStatus::RUNNING;
    };
    // Holds the GPU lease; a recording started meanwhile decodes after it finishes
    const system::AwakeRequest awake(L"ClinicAVT: writing the note");
    const auto lease = impl_->TakeGpu("note");
    if (lease.Waited() > 0.25) {
        log::Printf("clinicavt-note-host: generation waited %.2f s for the GPU lease\n",
                    lease.Waited());
    }
    pipeline->Generate(wrapped, Greedy(max_new_tokens), streamer);
    return Trimmed(text);
}

void LlmNoteWriter::Cancel() {
    impl_->cancel = true;
}

void LlmNoteWriter::Close() {
    impl_->closed = true;
    impl_->cancel = true;
}

}  // namespace clinicavt::note
