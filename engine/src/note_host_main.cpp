// Runs the note model in its own process, so a driver fault only costs a respawn. Speaks JSON-RPC
// over a private pipe and exits when the engine disconnects
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

#include "adapters/ipc/messages.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/note/llm_note_writer.hpp"
#include "adapters/system/power_throttling.hpp"
#include "adapters/system/stderr_log.hpp"
#include "core/common/log.hpp"
#include "core/note/model_failure.hpp"
#include "ports/transcriber.hpp"

namespace {

// Works around OpenVINO, where any GPU call can hang after a driver fault. Reports and exits at
// once without teardown
void ExitIfPoisoned(const std::string& detail) {
    if (!clinicavt::note::PoisonsGpuContext(detail)) return;
    clinicavt::log::Printf("clinicavt-note-host: the GPU context is corrupt (%.200s); exiting\n",
                           detail.c_str());
    std::fflush(stderr);
    std::_Exit(3);
}

// One generation at a time, off the RPC thread, so partials stream and cancel
// still gets through
class GenerationLane {
   public:
    explicit GenerationLane(clinicavt::ipc::PipeServer& server) : server_(server) {}

    ~GenerationLane() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Refused while another generation runs
    std::variant<clinicavt::ipc::json, clinicavt::ipc::Error> Start(
        std::function<std::string(const clinicavt::note::INoteWriter::Progress&)> generate) {
        if (running_.exchange(true)) {
            return clinicavt::ipc::SessionError("a generation is running");
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        thread_ = std::thread([this, generate = std::move(generate)] {
            try {
                const auto t0 = std::chrono::steady_clock::now();
                auto first_at = t0;
                std::size_t pieces = 0;  // one streamed piece per decoded token
                const std::string text =
                    generate([this, &t0, &first_at, &pieces](const std::string& partial) {
                        if (pieces++ == 0) {
                            first_at = std::chrono::steady_clock::now();
                            clinicavt::log::Printf(
                                "clinicavt-note-host: first token in %.1f s\n",
                                std::chrono::duration<double>(first_at - t0).count());
                        }
                        server_.PushNotification("partial", {{"text", partial}});
                    });
                const double decode_s =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - first_at)
                        .count();
                if (pieces > 1 && decode_s > 0) {
                    clinicavt::log::Printf(
                        "clinicavt-note-host: %zu tokens in %.1f s, %.1f tok/s\n", pieces, decode_s,
                        (pieces - 1) / decode_s);
                }
                server_.PushNotification("ready", {{"text", text}});
            } catch (const std::exception& e) {
                server_.PushNotification("failed", {{"detail", e.what()}});
                ExitIfPoisoned(e.what());
            } catch (...) {
                server_.PushNotification("failed", {{"detail", "note generation failed"}});
            }
            running_ = false;
        });
        return clinicavt::ipc::json::object();
    }

   private:
    clinicavt::ipc::PipeServer& server_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

std::vector<clinicavt::asr::Turn> TurnsFrom(const nlohmann::json& params) {
    return clinicavt::ipc::TurnsFromJson(params.value("turns", nlohmann::json::array()));
}

// The engine sends only names it validated. Anything else gets the default
clinicavt::note::NoteStyle StyleFrom(const nlohmann::json& params) {
    return clinicavt::note::NoteStyleFrom(params.value("style", "prose"))
        .value_or(clinicavt::note::NoteStyle::kProse);
}

clinicavt::note::NoteDetail DetailFrom(const nlohmann::json& params) {
    return clinicavt::note::NoteDetailFrom(params.value("detail", "concise"))
        .value_or(clinicavt::note::NoteDetail::kConcise);
}

}  // namespace

int main(int argc, char* argv[]) {
    clinicavt::system::LogToStderr();
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    try {
        if (argc < 4) {
            std::fprintf(stderr,
                         "usage: clinicavt_note_host <pipe> <models> <prompts-dir> [tier]\n");
            return 2;
        }
        clinicavt::log::Printf(
            "clinicavt-note-host: power throttling %s\n",
            clinicavt::system::Describe(clinicavt::system::DisableThrottlingOnSelf()).c_str());
        const std::wstring pipe_name = std::filesystem::path(argv[1]).wstring();
        const std::filesystem::path models_root = argv[2];
        const std::filesystem::path prompt_path = argv[3];
        // Tier is a role resolved by this process's model store
        const std::string tier = argc > 4 ? argv[4] : "default";

        clinicavt::ipc::PipeServer server(pipe_name);
        clinicavt::models::ModelStore store(models_root);
        clinicavt::models::OvRuntime runtime;
        using clinicavt::ipc::json;
        // The engine watches these two events to supervise the load
        const auto on_load = [&server](const clinicavt::note::LlmNoteWriter::LoadReport& r) {
            if (r.ok) {
                server.PushNotification("loaded", {{"id", r.id},
                                                   {"name", r.name},
                                                   {"seconds", r.seconds},
                                                   {"firstUse", r.first_use}});
            } else {
                server.PushNotification("loadFailed",
                                        {{"id", r.id}, {"name", r.name}, {"detail", r.detail}});
                ExitIfPoisoned(r.detail);
            }
        };
        // Any frame is a liveness signal, so waiting behind another engine is not
        // taken for a hang
        const auto on_gpu_wait = [&server](double waited) {
            server.PushNotification("gpuWait", {{"seconds", waited}});
        };
        clinicavt::note::LlmNoteWriter writer(store, runtime, prompt_path, tier, on_load,
                                              on_gpu_wait);
        GenerationLane lane(server);
        server.RegisterMethod("prepare", [&writer](const json&) {
            writer.Prepare();
            return json::object();
        });
        server.RegisterMethod("cancel", [&writer](const json&) {
            writer.Cancel();
            return json::object();
        });
        // Runs inline because it is short and a failure only loses a guess. The engine could
        // mistake a returned error for the reply to its next request
        server.RegisterMethod("prefill", [&writer](const json& params) {
            try {
                writer.Prefill(TurnsFrom(params),
                               {StyleFrom(params), clinicavt::note::NoteDetail::kConcise});
            } catch (const std::exception& e) {
                clinicavt::log::Printf("clinicavt-note-host: prefill dropped (%s)\n", e.what());
                ExitIfPoisoned(e.what());
            }
            return json::object();
        });
        server.RegisterMethod("write", [&writer, &lane](const json& params) {
            clinicavt::note::NoteOptions options{StyleFrom(params), DetailFrom(params)};
            options.confirmed = params.value("confirmed", false);
            return lane.Start([&writer, turns = TurnsFrom(params), options](const auto& progress) {
                return writer.Write(turns, options, progress);
            });
        });
        server.RegisterMethod("label", [&writer, &lane](const json& params) {
            return lane.Start([&writer, note = params.value("note", "")](const auto&) {
                return writer.WriteLabel(note);
            });
        });
        server.RegisterMethod("summary", [&writer, &lane](const json& params) {
            return lane.Start([&writer, note = params.value("note", "")](const auto&) {
                return writer.WriteSummary(note);
            });
        });
        server.RegisterMethod("writePatient", [&writer, &lane](const json& params) {
            return lane.Start([&writer, note = params.value("note", "")](const auto& progress) {
                return writer.WritePatient(note, progress);
            });
        });

        // Serves one client, the engine. When it disconnects the loop ends and any generation is
        // cancelled, so the host exits within seconds. A running load cannot be cancelled and
        // finishes first
        server.ServeOneClient();
        writer.Close();
        return 0;
    } catch (const std::exception& e) {
        clinicavt::log::Printf("clinicavt-note-host: fatal: %s\n", e.what());
        return 1;
    }
}
