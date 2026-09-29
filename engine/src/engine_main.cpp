#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

#include "adapters/archive/archive_lane.hpp"
#include "adapters/audio/media_foundation_reader.hpp"
#include "adapters/audio/wasapi_capture.hpp"
#include "adapters/audio/wav_source.hpp"
#include "adapters/demo/json_sample_source.hpp"
#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/guidance/document_ingest.hpp"
#include "adapters/guidance/embedder.hpp"
#include "adapters/guidance/guidance_lane.hpp"
#include "adapters/guidance/retriever.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/ipc/wire_events.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/note/worker_note_writer.hpp"
#include "adapters/storage/reflection_json.hpp"
#include "adapters/storage/sqlite_session_store.hpp"
#include "adapters/system/exe_paths.hpp"
#include "adapters/system/gpu_lease.hpp"
#include "adapters/system/power_throttling.hpp"
#include "adapters/system/stderr_log.hpp"
#include "adapters/transcription/whisper_transcriber.hpp"
#include "adapters/translate/nllb_translator.hpp"
#include "adapters/translate/translate_lane.hpp"
#include "composition/engine_config.hpp"
#include "composition/guidelines_setup.hpp"
#include "composition/model_roles.hpp"
#include "core/common/log.hpp"
#include "core/common/utf8.hpp"
#include "core/demo/sample_year.hpp"
#include "core/metrics/metrics.hpp"
#include "core/records/reflections.hpp"
#include "core/records/session_records.hpp"
#include "core/session/session_controller.hpp"

namespace {

// Another engine already owns the pipe; the shell reconnects to it instead of
// counting a crash
constexpr int kExitAlreadyServing = 3;

// Exit after this long with no shell connected
constexpr auto kIdleExit = std::chrono::seconds(30);

// 10 s because a Bluetooth microphone link takes 1.6-8.8 s to wake before
// first audio. Wired mics answer in well under a second
constexpr auto kSettleTimeout = std::chrono::seconds(10);

// Replay requests play a wav through the same port. A wav path given at launch
// (CI, scripts) forces every session to replay it
clinicavt::session::SourceFactory MakeSourceFactory(std::string forced) {
    return [forced = std::move(forced)](
               const std::optional<clinicavt::session::ReplaySpec>& replay,
               const std::string& mic_id) -> std::unique_ptr<clinicavt::audio::IAudioSource> {
        if (replay.has_value()) {
            return std::make_unique<clinicavt::audio::WavSource>(
                replay->path,
                clinicavt::audio::WavSource::Config{replay->speed, replay->start_frame});
        }
        if (!forced.empty()) return std::make_unique<clinicavt::audio::WavSource>(forced);
        return std::make_unique<clinicavt::audio::WasapiCapture>(clinicavt::audio::WideId(mic_id));
    };
}

// Lets a long GPU wait check whether the holder is our own host. Cleared before
// the lane is destroyed, since Whisper outlives it
class ProbeScope {
   public:
    explicit ProbeScope(clinicavt::note::WorkerNoteWriter* lane) {
        if (lane == nullptr) return;
        clinicavt::system::GpuLease::Global().SetStuckProbe(
            [lane] { return lane->CheckForStuckHost(); });
    }
    ~ProbeScope() {
        clinicavt::system::GpuLease::Global().SetStuckProbe({});
    }
    ProbeScope(const ProbeScope&) = delete;
    ProbeScope& operator=(const ProbeScope&) = delete;
};

// Closing the shell ends capture; a reopened shell reconnects. Stay while a
// note model is loading or ASR is switching device (neither can be cancelled).
// Otherwise exit when idle with no client, immediately if the shell asked
void Serve(clinicavt::ipc::PipeServer& server, clinicavt::session::SessionController& controller,
           const std::function<bool()>& busy) {
    bool exit_asked = false;
    std::atomic<bool> asked_now{false};
    server.RegisterMethod("engine/exit", [&asked_now](const nlohmann::json&) {
        asked_now = true;
        return nlohmann::json::object();
    });
    while (server.AwaitClient(exit_asked ? std::chrono::seconds(0) : kIdleExit, busy) ==
           clinicavt::ipc::PipeServer::Accept::kClient) {
        asked_now = false;
        // Only a client that sent a frame changes exit_asked; a stale connect that
        // leaves cannot cancel a requested exit
        if (server.Serve()) exit_asked = asked_now;
        controller.Stop();
    }
}

}  // namespace

// Wide, so a name or path outside the ANSI code page arrives intact. Arguments are UTF-8 from here
int wmain(int argc, wchar_t* argv[]) {
    clinicavt::system::LogToStderr();
#ifdef _DEBUG
    // Send asserts and CRT errors to stderr instead of a modal dialog (headless process)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    namespace composition = clinicavt::composition;
    try {
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) args.push_back(clinicavt::utf8::FromPath(argv[i]));
        const composition::EngineConfig config = composition::ParseConfig(std::move(args));
        // All engines and note hosts share one GPU lease name, passed to hosts in the environment
        _putenv_s("CLINICAVT_GPU_LEASE", clinicavt::system::kGpuLeaseName);
        clinicavt::log::Printf(
            "clinicavt-engine: power throttling %s\n",
            clinicavt::system::Describe(clinicavt::system::DisableThrottlingOnSelf()).c_str());

        clinicavt::ipc::PipeServer server(config.pipe_name);
        clinicavt::store::SqliteSessionStore session_store(config.store_root);
        clinicavt::archive::ArchiveLane archive_lane(session_store, clinicavt::ipc::PushTo(server));
        // Retain-off sessions are erased even if the app was closed mid-consultation
        session_store.EraseUnretained();
        clinicavt::models::ModelStore model_store(config.models_root);
        clinicavt::models::OvRuntime ov_runtime;
        clinicavt::metrics::Registry metrics;
        clinicavt::diar::AnchorStore anchors(config.store_root);

        const bool stray_note_host = composition::FindStrayNoteHost();
        composition::RoleReport roles;
        auto transcriber = composition::BuildTranscriber(model_store, ov_runtime, config.asr_device,
                                                         metrics, config.scripted, roles);
        auto vad = composition::BuildVad(model_store, ov_runtime, metrics, config.scripted, roles);
        auto diariser = composition::BuildDiariser(model_store, ov_runtime, anchors, metrics,
                                                   config.scripted, roles);
        const std::string auto_tier = composition::MachineNoteTier(model_store);
        // The shell re-sends its tier on connect; a no-op by then
        auto note_writer = composition::BuildNoteWriter(
            model_store, config.models_root, config.note_tier, auto_tier,
            [&server](const clinicavt::note::NoteModelState& state) {
                server.PushNotification("note/model", clinicavt::ipc::NoteModelJson(state));
            },
            roles);
        // Report a stuck host at startup instead of on the first note request
        if (stray_note_host && note_writer != nullptr) note_writer->Prepare();
        const ProbeScope probe_scope(note_writer.get());
        auto translator = composition::BuildTranslator(model_store, ov_runtime, roles);
        // Guidance runs on the CPU in its own lane; the embedder loads in the
        // background so the first note search is warm
        clinicavt::guidance::Retriever guidance_retriever(
            [&model_store]() -> std::unique_ptr<clinicavt::guidance::IEmbedder> {
                return clinicavt::guidance::Embedder::Load(model_store);
            },
            config.corpora_root,
            clinicavt::guidance::RetrieverOptions{.include_research = config.include_research});
        clinicavt::guidance::GuidanceLane guidance_lane(
            guidance_retriever, [&server](const clinicavt::guidance::Readiness& readiness) {
                server.PushNotification("guidance/model",
                                        clinicavt::ipc::GuidanceModelJson(readiness));
            });
        clinicavt::ipc::WireEvents events(server, session_store, translator.get(), &guidance_lane);
        std::unique_ptr<clinicavt::translate::TranslateLane> translate_lane;
        if (translator != nullptr) {
            translate_lane = std::make_unique<clinicavt::translate::TranslateLane>(
                *translator, [&events](const std::string& method, const nlohmann::json& params) {
                    events.OnTranslation(method, params);
                });
        }
        guidance_lane.Prepare();

        clinicavt::session::SessionController controller(
            MakeSourceFactory(config.replay_wav), events, session_store, *transcriber.port, *vad,
            *diariser, kSettleTimeout, std::uint64_t{5} * clinicavt::audio::kSampleRate,
            note_writer.get(), &metrics);

        // Added documents embed between note searches and pause during a consultation
        const auto guidelines =
            composition::PrepareGuidelinesFolder(config.guidelines_override, config.models_root);
        const auto ingest_host = clinicavt::system::ExeDir() / clinicavt::system::kIngestHostExe;
        clinicavt::guidance::DocumentIngest ingest(
            guidance_retriever, guidelines, config.store_root / "documents",
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
        clinicavt::audio::MediaFoundationReader recordings;
        auto* const whisper = transcriber.whisper;
        const clinicavt::ipc::AsrSwitch switch_asr =
            [whisper](const std::string& device, std::function<void(const std::string&)> done) {
                return whisper != nullptr && whisper->SwitchDevice(device, std::move(done));
            };
        clinicavt::store::JsonReflectionCodec reflection_codec;
        clinicavt::records::SessionRecords records(session_store);
        clinicavt::records::Reflections reflections(session_store, reflection_codec);
        clinicavt::demo::JsonSampleSource samples(config.models_root.parent_path() / "demo" /
                                                  "reflections");
        clinicavt::demo::DemoSamples demo(session_store, reflection_codec, samples);
        clinicavt::ipc::RegisterMethods(server, {.controller = controller,
                                                 .models = model_store,
                                                 .sessions = session_store,
                                                 .records = records,
                                                 .reflections = reflections,
                                                 .demo = demo,
                                                 .metrics = &metrics,
                                                 .runtime = &ov_runtime,
                                                 .translator = translator.get(),
                                                 .translate_lane = translate_lane.get(),
                                                 .first_use = roles.first_use,
                                                 .anchors = &anchors,
                                                 .note_tiers = note_writer.get(),
                                                 .auto_note_tier = auto_tier,
                                                 .stray_note_host = stray_note_host,
                                                 .switch_asr = switch_asr,
                                                 .archive_lane = &archive_lane,
                                                 .recordings = &recordings,
                                                 .missing_models = roles.missing,
                                                 .allow_replay = config.allow_replay});
        clinicavt::ipc::RegisterGuidanceMethods(server, session_store, guidance_retriever,
                                                guidance_lane, ingest);
        Serve(server, controller, [&note_writer, whisper] {
            return (note_writer != nullptr &&
                    note_writer->State().phase ==
                        clinicavt::note::NoteModelState::Phase::kLoading) ||
                   (whisper != nullptr && whisper->Moving());
        });
        return 0;
    } catch (const clinicavt::ipc::PipeTaken& e) {
        clinicavt::log::Printf("clinicavt-engine: %s\n", e.what());
        return kExitAlreadyServing;
    } catch (const std::exception& e) {
        clinicavt::log::Printf("clinicavt-engine: %s\n", e.what());
        return 1;
    }
}
