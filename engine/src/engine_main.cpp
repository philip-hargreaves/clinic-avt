#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <shlobj.h>
#include <windows.h>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

#include "adapters/archive/archive_lane.hpp"
#include "adapters/audio/capture_devices.hpp"
#include "adapters/audio/media_foundation_reader.hpp"
#include "adapters/audio/wasapi_capture.hpp"
#include "adapters/audio/wav_source.hpp"
#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/diarisation/deferred_diariser.hpp"
#include "adapters/diarisation/scripted_diariser.hpp"
#include "adapters/diarisation/speaker_diariser.hpp"
#include "adapters/guidance/document_ingest.hpp"
#include "adapters/guidance/embedder.hpp"
#include "adapters/guidance/guidance_lane.hpp"
#include "adapters/guidance/retriever.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/ipc/wire_events.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/models/note_tier.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/note/worker_note_writer.hpp"
#include "adapters/storage/sqlite_session_store.hpp"
#include "adapters/system/exe_paths.hpp"
#include "adapters/system/gpu_lease.hpp"
#include "adapters/system/machine_memory.hpp"
#include "adapters/system/power_throttling.hpp"
#include "adapters/system/process_scan.hpp"
#include "adapters/transcription/scripted_transcriber.hpp"
#include "adapters/transcription/whisper_transcriber.hpp"
#include "adapters/translate/nllb_translator.hpp"
#include "adapters/translate/translate_lane.hpp"
#include "adapters/vad/deferred_vad.hpp"
#include "adapters/vad/passthrough_vad.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "core/common/cli_args.hpp"
#include "core/metrics/metrics.hpp"
#include "core/session/playback.hpp"
#include "core/session/session_controller.hpp"

namespace {

// Another engine already serves the pipe. The shell takes it over rather
// than counting a crash
constexpr int kExitAlreadyServing = 3;

// How long an engine waits for a shell to come back before it leaves
constexpr auto kIdleExit = std::chrono::seconds(30);

std::filesystem::path StoreRoot(const std::vector<std::string>& args) {
    if (args.size() > 1) return args[1];
    char* local_app_data = nullptr;
    if (_dupenv_s(&local_app_data, nullptr, "LOCALAPPDATA") != 0 || local_app_data == nullptr) {
        throw std::runtime_error("LOCALAPPDATA is not set and no store root was given");
    }
    const auto root = std::filesystem::path(local_app_data) / "ClinicAVT" / "store";
    std::free(local_app_data);
    return root;
}

// Added documents live in the user's Documents folder unless a run says otherwise
std::filesystem::path GuidelinesFolder(const std::string& override) {
    if (!override.empty()) return override;
    PWSTR documents = nullptr;
    std::filesystem::path folder;
    if (SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents) == S_OK) {
        folder = std::filesystem::path(documents) / "ClinicAVT guidelines";
    }
    CoTaskMemFree(documents);
    if (folder.empty()) throw std::runtime_error("no Documents folder and no --guidelines given");
    return folder;
}

// True when a staged model has never been compiled on this machine
bool Uncompiled(const clinicavt::models::ModelStore& store, const std::string& role,
                const std::string& tier = "default") {
    return !clinicavt::models::Compiled(store.Resolve(role, tier));
}

// A replay request plays a wav through the same port. A launch-time wav path,
// used by CI and scripts, forces every session to replay that file
clinicavt::session::SourceFactory MakeSourceFactory(std::string forced) {
    return [forced = std::move(forced)](
               const std::optional<clinicavt::session::ReplaySpec>& replay,
               const std::string& mic_id) -> std::unique_ptr<clinicavt::audio::IAudioSource> {
        if (replay.has_value()) {
            return std::make_unique<clinicavt::audio::WavSource>(
                replay->path, clinicavt::audio::WavSource::Config{replay->speed, replay->monitor,
                                                                  replay->start_frame});
        }
        if (!forced.empty()) return std::make_unique<clinicavt::audio::WavSource>(forced);
        return std::make_unique<clinicavt::audio::WasapiCapture>(clinicavt::audio::WideId(mic_id));
    };
}

// Real transcription when the ASR role is staged, scripted otherwise as in CI
std::unique_ptr<clinicavt::asr::ITranscriber> BuildTranscriber(
    const clinicavt::models::ModelStore& store, clinicavt::models::OvRuntime& runtime,
    const std::string& device, clinicavt::metrics::Registry& metrics, bool& first_use) {
    try {
        first_use |= Uncompiled(store, "asr");
        return std::make_unique<clinicavt::asr::WhisperTranscriber>(store, runtime, device,
                                                                    &metrics);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt-engine: scripted transcripts (%s)\n", e.what());
        return std::make_unique<clinicavt::asr::ScriptedTranscriber>();
    }
}

// Compiles behind the serve loop. session/start waits on it, hello does not
std::unique_ptr<clinicavt::audio::IStreamingVad> BuildVad(
    const clinicavt::models::ModelStore& store, clinicavt::models::OvRuntime& runtime,
    clinicavt::metrics::Registry& metrics) {
    try {
        store.Resolve("vad", "default");
        return std::make_unique<clinicavt::audio::DeferredVad>(
            [&store, &runtime] {
                return std::make_unique<clinicavt::audio::SileroVad>(store, runtime);
            },
            &metrics);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt-engine: capped windows (%s)\n", e.what());
        return std::make_unique<clinicavt::audio::PassthroughVad>();
    }
}

// Diarisation needs both its models and is scripted otherwise, as in CI
std::unique_ptr<clinicavt::diar::IDiariser> BuildDiariser(
    const clinicavt::models::ModelStore& store, clinicavt::models::OvRuntime& runtime,
    clinicavt::diar::AnchorStore& anchors, clinicavt::metrics::Registry& metrics) {
    try {
        store.Resolve("diarisation", "default");
        store.Resolve("segmentation", "default");
        return std::make_unique<clinicavt::diar::DeferredDiariser>(
            [&store, &runtime, &anchors] {
                return std::make_unique<clinicavt::diar::SpeakerDiariser>(store, runtime, anchors);
            },
            &metrics);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt-engine: scripted speakers (%s)\n", e.what());
        return std::make_unique<clinicavt::diar::ScriptedDiariser>();
    }
}

// Generation runs in its own supervised process, so a GPU driver fault there
// costs a respawn and leaves the engine standing. Null when nothing can write
std::unique_ptr<clinicavt::note::WorkerNoteWriter> BuildNoteWriter(
    clinicavt::models::ModelStore& store, const std::filesystem::path& models_root,
    clinicavt::ipc::PipeServer& server, const std::string& requested_tier,
    const std::string& auto_tier, bool& first_use) {
    try {
        if (auto_tier.empty()) {
            std::fputs("clinicavt-engine: no note model staged\n", stderr);
            return nullptr;
        }
        // The shell's tier, so the one-off compile is of the model it will ask for. Automatic,
        // or a tier not staged here, starts on this machine's pick
        std::string tier = auto_tier;
        if (!requested_tier.empty() && requested_tier != clinicavt::models::kAutoNoteTier) {
            try {
                store.Resolve("note", requested_tier);
                tier = requested_tier;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "clinicavt-engine: note tier %s not staged (%s)\n",
                             requested_tier.c_str(), e.what());
            }
        }
        const auto host = clinicavt::system::ExeDir() / clinicavt::system::kNoteHostExe;
        if (!std::filesystem::exists(host)) {
            // Never write in-process, because that is the configuration the driver fault corrupts
            std::fprintf(stderr, "clinicavt-engine: note DISABLED, %s is missing\n",
                         host.string().c_str());
            return nullptr;
        }
        auto worker = std::make_unique<clinicavt::note::WorkerNoteWriter>(
            host, models_root, models_root.parent_path() / "prompts", &store, tier);
        // The shell configures the tier again on connect, which is then no change
        worker->SetListener([&server](const clinicavt::note::NoteModelState& state) {
            server.PushNotification("note/model", clinicavt::ipc::NoteModelJson(state));
        });
        // On first use the one-off compile runs on an idle GPU, ahead of any recording
        if (Uncompiled(store, "note", tier)) {
            first_use = true;
            std::fprintf(stderr, "clinicavt-engine: first use, compiling the note model\n");
            worker->Prepare();
        }
        return worker;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt-engine: stub note (%s)\n", e.what());
        return nullptr;
    }
}

// Translation runs on the CPU, so it never contends with the GPU. Null when
// the model is not staged
std::unique_ptr<clinicavt::translate::NllbTranslator> BuildTranslator(
    const clinicavt::models::ModelStore& store, clinicavt::models::OvRuntime& runtime,
    bool& first_use) {
    try {
        store.Resolve("translation", "default");
        auto translator = std::make_unique<clinicavt::translate::NllbTranslator>(store, runtime);
        // The CPU compile joins the one-off warm-up, so the first translation
        // is as fast as every other
        if (Uncompiled(store, "translation")) {
            first_use = true;
            std::fprintf(stderr, "clinicavt-engine: first use, compiling the translator\n");
            translator->Prepare();
        }
        return translator;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt-engine: no translation (%s)\n", e.what());
        return nullptr;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _DEBUG
    // Assertions and CRT errors go to stderr as text rather than parking a
    // headless engine behind a modal dialog
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    try {
        // Flags come first, then the positional pipe name, store root, models root and replay wav
        std::vector<std::string> args(argv + 1, argv + argc);
        const std::string asr_device = clinicavt::TakeFlag(args, "--asr-device");
        const std::string note_tier = clinicavt::TakeFlag(args, "--note-tier");
        const std::string corpora_override = clinicavt::TakeFlag(args, "--corpora");
        const std::string guidelines_override = clinicavt::TakeFlag(args, "--guidelines");
        // For dev builds, a demo corpus marked research is searched when set
        const bool include_research = clinicavt::TakeSwitch(args, "--include-research");
        // Every engine and note host takes turns on the GPU under one name,
        // which the hosts inherit
        _putenv_s("CLINICAVT_GPU_LEASE", clinicavt::system::kGpuLeaseName);
        std::fprintf(
            stderr, "clinicavt-engine: power throttling %s\n",
            clinicavt::system::Describe(clinicavt::system::DisableThrottlingOnSelf()).c_str());

        std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\clinicavt-engine";
        if (args.size() > 0) {
            pipe_name = L"\\\\.\\pipe\\" + std::wstring(args[0].begin(), args[0].end());
        }
        const std::filesystem::path store_root = StoreRoot(args);
        const std::filesystem::path models_root = args.size() > 2
                                                      ? std::filesystem::path(args[2])
                                                      : clinicavt::system::DefaultModelsRoot();
        // Guidance corpora sit beside the models, each replaced as a directory
        const std::filesystem::path corpora_root = corpora_override.empty()
                                                       ? models_root.parent_path() / "corpora"
                                                       : std::filesystem::path(corpora_override);

        clinicavt::ipc::PipeServer server(pipe_name);
        clinicavt::store::SqliteSessionStore session_store(store_root);
        clinicavt::ipc::WireEvents events(server, session_store);
        clinicavt::archive::ArchiveLane archive_lane(session_store, clinicavt::ipc::PushTo(server));
        // A consultation left by closing the app is left all the same
        session_store.EraseUnretained();
        clinicavt::models::ModelStore model_store(models_root);
        clinicavt::models::OvRuntime ov_runtime;
        clinicavt::metrics::Registry metrics;
        clinicavt::diar::AnchorStore anchors(store_root);

        // A host whose engine has gone takes a few seconds to leave. One still
        // here after that is stuck in the driver, and only a restart ends it.
        // Checked before any model touches the GPU
        const bool stray_note_host = !clinicavt::system::LingeringOrphans(
                                          clinicavt::system::kNoteHostExe, std::chrono::seconds(8))
                                          .empty();
        if (stray_note_host) {
            clinicavt::system::GpuLease::Global().MarkWedged();
            std::fprintf(stderr,
                         "clinicavt-engine: a note host from an earlier engine is stuck; "
                         "the GPU is not ours until the computer restarts\n");
        }
        bool first_use = false;
        auto transcriber =
            BuildTranscriber(model_store, ov_runtime, asr_device, metrics, first_use);
        auto vad = BuildVad(model_store, ov_runtime, metrics);
        auto diariser = BuildDiariser(model_store, ov_runtime, anchors, metrics);
        const clinicavt::models::MachineMemory memory{clinicavt::system::InstalledMemoryBytes(),
                                                      clinicavt::system::IntelGpuMemoryBytes()};
        const std::string auto_tier = clinicavt::models::AutoNoteTier(
            clinicavt::models::StagedNoteTiers(model_store), memory);
        constexpr double kGib = 1ULL << 30;
        std::fprintf(
            stderr,
            "clinicavt-engine: note model for this machine %s (memory %.1f GB, GPU %.1f GB)\n",
            auto_tier.empty() ? "none" : auto_tier.c_str(),
            static_cast<double>(memory.installed.value_or(0)) / kGib,
            static_cast<double>(memory.gpu.value_or(0)) / kGib);
        auto note_writer =
            BuildNoteWriter(model_store, models_root, server, note_tier, auto_tier, first_use);
        // The lane says so at once rather than when a note is asked for
        if (stray_note_host && note_writer != nullptr) note_writer->Prepare();
        // A GPU wait that runs on asks whether the holder is this engine's own
        // host. Cleared before the lane goes, since Whisper outlives it
        struct ProbeScope {
            explicit ProbeScope(clinicavt::note::WorkerNoteWriter* lane) {
                if (lane == nullptr) return;
                clinicavt::system::GpuLease::Global().SetStuckProbe(
                    [lane] { return lane->CheckForStuckHost(); });
            }
            ~ProbeScope() {
                clinicavt::system::GpuLease::Global().SetStuckProbe({});
            }
        } probe_scope(note_writer.get());
        auto translator = BuildTranslator(model_store, ov_runtime, first_use);
        std::unique_ptr<clinicavt::translate::TranslateLane> translate_lane;
        if (translator != nullptr) {
            translate_lane = std::make_unique<clinicavt::translate::TranslateLane>(
                *translator, [&events](const std::string& method, const nlohmann::json& params) {
                    events.OnTranslation(method, params);
                });
            events.SetTranslator(translator.get());
        }
        // Guidance retrieval runs on the CPU in its own lane. The embedder loads
        // in the background so the first note's search is warm
        clinicavt::guidance::Retriever guidance_retriever(
            [&model_store]() -> std::unique_ptr<clinicavt::guidance::IEmbedder> {
                return clinicavt::guidance::Embedder::Load(model_store);
            },
            corpora_root,
            clinicavt::guidance::RetrieverOptions{.include_research = include_research});
        clinicavt::guidance::GuidanceLane guidance_lane(
            guidance_retriever, [&server](const clinicavt::guidance::Readiness& readiness) {
                server.PushNotification("guidance/model",
                                        clinicavt::ipc::GuidanceModelJson(readiness));
            });
        events.SetGuidance(&guidance_lane);
        guidance_lane.Prepare();

        // 10 s because a Bluetooth microphone link takes 1.6-8.8 s to wake before
        // first audio. Wired mics answer in well under a second
        clinicavt::session::SessionController controller(
            MakeSourceFactory(args.size() > 3 ? args[3] : std::string()), events, session_store,
            *transcriber, *vad, *diariser, std::chrono::seconds(10),
            5 * clinicavt::audio::kSampleRate, note_writer.get(), &metrics);

        // Added documents embed between note searches and wait while a consultation runs
        const auto ingest_host = clinicavt::system::ExeDir() / clinicavt::system::kIngestHostExe;
        clinicavt::guidance::DocumentIngest ingest(
            guidance_retriever, GuidelinesFolder(guidelines_override), store_root / "documents",
            [&controller] { return controller.Running(); },
            std::filesystem::exists(ingest_host) ? ingest_host : std::filesystem::path());
        ingest.SetListener(
            [&server](const clinicavt::guidance::IngestProgress& progress) {
                server.PushNotification("guidance/progress",
                                        clinicavt::ipc::ProgressJson(progress));
            },
            [&server](const clinicavt::guidance::DocumentInfo& document) {
                server.PushNotification("guidance/document",
                                        clinicavt::ipc::DocumentJson(document));
                if (clinicavt::ipc::ChangesReadySet(document)) {
                    server.PushNotification("guidance/documentsChanged", nlohmann::json::object());
                }
            });
        // Demo playback ends in a review of the copy, its note searched like any other
        clinicavt::session::Playback playback(
            events, session_store,
            {.finalised = [&controller](const std::string& id) { controller.Open(id); },
             .guidance =
                 [&events, &session_store](const std::string& id) {
                     const auto note =
                         session_store.ReadDocument(id, clinicavt::store::DocumentKind::kNote);
                     if (!note.text.empty()) events.OnNoteSaved(id, note);
                 }});
        clinicavt::audio::MediaFoundationReader recordings;
        clinicavt::ipc::RegisterMethods(
            server,
            {.controller = controller,
             .models = model_store,
             .sessions = session_store,
             .metrics = &metrics,
             .runtime = &ov_runtime,
             .translator = translator.get(),
             .translate_lane = translate_lane.get(),
             .first_use = first_use,
             .anchors = &anchors,
             .note_lane = note_writer.get(),
             .auto_note_tier = auto_tier,
             .stray_note_host = stray_note_host,
             .demo_dir = models_root.parent_path() / "demo" / "reflections",
             .playback = &playback,
             .switch_asr =
                 [whisper = dynamic_cast<clinicavt::asr::WhisperTranscriber*>(transcriber.get())](
                     const std::string& device, std::function<void(const std::string&)> done) {
                     return whisper != nullptr && whisper->SwitchDevice(device, std::move(done));
                 },
             .archive_lane = &archive_lane,
             .recordings = &recordings});
        clinicavt::ipc::RegisterGuidanceMethods(server, session_store, guidance_retriever,
                                                guidance_lane, ingest);
        // A shell that closes ends its capture, and a reopened one picks this
        // engine up again. A note model still loading or speech recognition
        // moving device keeps it here, since neither load can be cancelled.
        // Idle and alone, it leaves, at once when the shell asked it to
        bool exit_asked = false;
        std::atomic<bool> asked_now{false};
        server.RegisterMethod("engine/exit", [&asked_now](const nlohmann::json&) {
            asked_now = true;
            return nlohmann::json::object();
        });
        const auto busy =
            [&note_writer,
             whisper = dynamic_cast<clinicavt::asr::WhisperTranscriber*>(transcriber.get())] {
                return (note_writer != nullptr &&
                        note_writer->State().phase ==
                            clinicavt::note::NoteModelState::Phase::kLoading) ||
                       (whisper != nullptr && whisper->Moving());
            };
        while (server.AwaitClient(exit_asked ? std::chrono::seconds(0) : kIdleExit, busy) ==
               clinicavt::ipc::PipeServer::Accept::kClient) {
            asked_now = false;
            // Only a client that speaks decides. A stale dial that touches the
            // pipe and leaves never cancels an exit already asked for
            if (server.Serve()) exit_asked = asked_now;
            controller.Stop();
        }
        return 0;
    } catch (const clinicavt::ipc::PipeTaken& e) {
        std::fprintf(stderr, "clinicavt-engine: %s\n", e.what());
        return kExitAlreadyServing;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt-engine: %s\n", e.what());
        return 1;
    }
}
