#include "composition/model_roles.hpp"

#include <chrono>
#include <exception>
#include <initializer_list>
#include <utility>

#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/diarisation/deferred_diariser.hpp"
#include "adapters/diarisation/scripted_diariser.hpp"
#include "adapters/diarisation/speaker_diariser.hpp"
#include "adapters/models/missing_models.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/models/note_tier.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/note/worker_note_writer.hpp"
#include "adapters/system/exe_paths.hpp"
#include "adapters/system/gpu_lease.hpp"
#include "adapters/system/machine_memory.hpp"
#include "adapters/system/process_scan.hpp"
#include "adapters/transcription/scripted_transcriber.hpp"
#include "adapters/transcription/whisper_transcriber.hpp"
#include "adapters/translate/nllb_translator.hpp"
#include "adapters/vad/deferred_vad.hpp"
#include "adapters/vad/passthrough_vad.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "core/common/log.hpp"
#include "core/metrics/metrics.hpp"

namespace clinicavt::composition {

namespace {

bool Uncompiled(const models::ModelStore& store, const std::string& role,
                const std::string& tier = "default") {
    return !models::Compiled(store.Resolve(role, tier));
}

// The roles the store cannot resolve
std::vector<std::string> Unstaged(const models::ModelStore& store,
                                  std::initializer_list<const char*> roles) {
    std::vector<std::string> unstaged;
    for (const char* role : roles) {
        try {
            store.Resolve(role, "default");
        } catch (const std::exception&) {
            unstaged.emplace_back(role);
        }
    }
    return unstaged;
}

}  // namespace

Transcriber BuildTranscriber(const models::ModelStore& store, models::OvRuntime& runtime,
                             const std::string& device, metrics::Registry& metrics, bool scripted,
                             RoleReport& report) {
    try {
        report.first_use |= Uncompiled(store, "asr");
        auto whisper = std::make_unique<asr::WhisperTranscriber>(store, runtime, device, &metrics);
        auto* const concrete = whisper.get();
        return {std::move(whisper), concrete};
    } catch (const std::exception& e) {
        if (scripted) {
            log::Printf("clinicavt-engine: scripted transcripts (%s)\n", e.what());
            return {std::make_unique<asr::ScriptedTranscriber>(), nullptr};
        }
        log::Printf("clinicavt-engine: no speech recognition (%s)\n", e.what());
        report.missing.emplace_back("asr");
        return {std::make_unique<models::MissingTranscriber>(), nullptr};
    }
}

std::unique_ptr<audio::IStreamingVad> BuildVad(const models::ModelStore& store,
                                               models::OvRuntime& runtime,
                                               metrics::Registry& metrics, bool scripted,
                                               RoleReport& report) {
    if (Unstaged(store, {"vad"}).empty()) {
        return std::make_unique<audio::DeferredVad>(
            [&store, &runtime] { return std::make_unique<audio::SileroVad>(store, runtime); },
            &metrics);
    }
    if (scripted) {
        log::Printf("clinicavt-engine: capped windows, no speech detection model\n");
        return std::make_unique<audio::PassthroughVad>();
    }
    log::Printf("clinicavt-engine: no speech detection model\n");
    report.missing.emplace_back("vad");
    return std::make_unique<models::MissingVad>();
}

std::unique_ptr<diar::IDiariser> BuildDiariser(const models::ModelStore& store,
                                               models::OvRuntime& runtime,
                                               diar::AnchorStore& anchors,
                                               metrics::Registry& metrics, bool scripted,
                                               RoleReport& report) {
    const auto unstaged = Unstaged(store, {"diarisation", "segmentation"});
    if (unstaged.empty()) {
        return std::make_unique<diar::DeferredDiariser>(
            [&store, &runtime, &anchors] {
                return std::make_unique<diar::SpeakerDiariser>(store, runtime, anchors);
            },
            &metrics);
    }
    if (scripted) {
        log::Printf("clinicavt-engine: scripted speakers, no speaker models\n");
        return std::make_unique<diar::ScriptedDiariser>();
    }
    log::Printf("clinicavt-engine: no speaker models\n");
    report.missing.insert(report.missing.end(), unstaged.begin(), unstaged.end());
    return std::make_unique<models::MissingDiariser>();
}

std::string MachineNoteTier(const models::ModelStore& store) {
    const models::MachineMemory memory{system::InstalledMemoryBytes(),
                                       system::IntelGpuMemoryBytes()};
    const std::string auto_tier = models::AutoNoteTier(models::StagedNoteTiers(store), memory);
    constexpr double kGib = 1ULL << 30;
    log::Printf("clinicavt-engine: note model for this machine %s (memory %.1f GB, GPU %.1f GB)\n",
                auto_tier.empty() ? "none" : auto_tier.c_str(),
                static_cast<double>(memory.installed.value_or(0)) / kGib,
                static_cast<double>(memory.gpu.value_or(0)) / kGib);
    return auto_tier;
}

std::unique_ptr<note::WorkerNoteWriter> BuildNoteWriter(models::ModelStore& store,
                                                        const std::filesystem::path& models_root,
                                                        const std::string& requested_tier,
                                                        const std::string& auto_tier,
                                                        note::INoteTiers::Listener listener,
                                                        RoleReport& report) {
    try {
        if (auto_tier.empty()) {
            log::Printf("clinicavt-engine: no note model staged\n");
            return nullptr;
        }
        // Start on the tier the shell will request; auto or unstaged uses the machine default
        std::string tier = auto_tier;
        if (!requested_tier.empty() && requested_tier != models::kAutoNoteTier) {
            try {
                store.Resolve("note", requested_tier);
                tier = requested_tier;
            } catch (const std::exception& e) {
                log::Printf("clinicavt-engine: note tier %s not staged (%s)\n",
                            requested_tier.c_str(), e.what());
            }
        }
        const auto host = system::ExeDir() / system::kNoteHostExe;
        if (!std::filesystem::exists(host)) {
            // Never generate in-process; that is the configuration the driver fault corrupts
            log::Printf("clinicavt-engine: note DISABLED, %s is missing\n", host.string().c_str());
            return nullptr;
        }
        auto worker = std::make_unique<note::WorkerNoteWriter>(
            host, models_root, models_root.parent_path() / "prompts", &store, tier,
            std::move(listener));
        // First use: compile on an idle GPU before any recording
        if (Uncompiled(store, "note", tier)) {
            report.first_use = true;
            log::Printf("clinicavt-engine: first use, compiling the note model\n");
            worker->Prepare();
        }
        return worker;
    } catch (const std::exception& e) {
        log::Printf("clinicavt-engine: stub note (%s)\n", e.what());
        return nullptr;
    }
}

std::unique_ptr<translate::NllbTranslator> BuildTranslator(const models::ModelStore& store,
                                                           models::OvRuntime& runtime,
                                                           RoleReport& report) {
    try {
        store.Resolve("translation", "default");
        auto translator = std::make_unique<translate::NllbTranslator>(store, runtime);
        // Compile during the startup warm-up so the first translation is not slower
        if (Uncompiled(store, "translation")) {
            report.first_use = true;
            log::Printf("clinicavt-engine: first use, compiling the translator\n");
            translator->Prepare();
        }
        return translator;
    } catch (const std::exception& e) {
        log::Printf("clinicavt-engine: no translation (%s)\n", e.what());
        return nullptr;
    }
}

bool FindStrayNoteHost() {
    const bool stray =
        !system::LingeringOrphans(system::kNoteHostExe, std::chrono::seconds(8)).empty();
    if (stray) {
        system::GpuLease::Global().MarkWedged();
        log::Printf(
            "clinicavt-engine: a note host from an earlier engine is stuck; "
            "the GPU is not ours until the computer restarts\n");
    }
    return stray;
}

}  // namespace clinicavt::composition
