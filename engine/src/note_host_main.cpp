// The note model's own process: a driver fault here costs a respawn, never
// the engine. Speaks JSON-RPC on a private pipe and exits when the engine goes.
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
#include "core/note/model_failure.hpp"
#include "ports/transcriber.hpp"

namespace {

// OpenVINO's own workaround: after a driver fault any further GPU call can
// hang the process, so it reports and leaves at once, before any teardown
void ExitIfPoisoned(const std::string& detail) {
    if (!clinicavt::note::PoisonsGpuContext(detail)) return;
    std::fprintf(stderr, "clinicavt-note-host: the GPU context is corrupt (%.200s); exiting\n",
                 detail.c_str());
    std::fflush(stderr);
    std::_Exit(3);
}

// One generation at a time, off the RPC thread so partials stream while
// the pipe stays responsive to cancel
class GenerationLane {
   public:
    explicit GenerationLane(clinicavt::ipc::PipeServer& server) : server_(server) {}

    ~GenerationLane() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // The reply to the request: refused while another generation runs
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
                            std::fprintf(stderr, "clinicavt-note-host: first token in %.1f s\n",
                                         std::chrono::duration<double>(first_at - t0).count());
                        }
                        server_.PushNotification("partial", {{"text", partial}});
                    });
                const double decode_s =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - first_at)
                        .count();
                if (pieces > 1 && decode_s > 0) {
                    std::fprintf(stderr, "clinicavt-note-host: %zu tokens in %.1f s, %.1f tok/s\n",
                                 pieces, decode_s, (pieces - 1) / decode_s);
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

}  // namespace

int main(int argc, char* argv[]) {
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
        std::fprintf(
            stderr, "clinicavt-note-host: power throttling %s\n",
            clinicavt::system::Describe(clinicavt::system::DisableThrottlingOnSelf()).c_str());
        const std::wstring pipe_name = std::filesystem::path(argv[1]).wstring();
        const std::filesystem::path models_root = argv[2];
        const std::filesystem::path prompt_path = argv[3];
        // The tier is a role that the store inside this process resolves
        const std::string tier = argc > 4 ? argv[4] : "default";

        clinicavt::ipc::PipeServer server(pipe_name);
        clinicavt::models::ModelStore store(models_root);
        clinicavt::models::OvRuntime runtime;
        clinicavt::note::LlmNoteWriter writer(store, runtime, prompt_path, tier);
        GenerationLane lane(server);

        using clinicavt::ipc::json;
        // The engine supervises the load through these two events
        writer.SetLoadListener([&server](const clinicavt::note::LlmNoteWriter::LoadReport& r) {
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
        });
        // Any frame tells the engine the host is alive, so a wait behind another
        // engine's work is never taken for a hang
        writer.SetGpuWaitListener([&server](double waited) {
            server.PushNotification("gpuWait", {{"seconds", waited}});
        });
        server.RegisterMethod("prepare", [&writer](const json&) {
            writer.Prepare();
            return json::object();
        });
        server.RegisterMethod("cancel", [&writer](const json&) {
            writer.Cancel();
            return json::object();
        });
        // Inline: short, and a failure is only a lost guess. Reporting it as an
        // error would let the engine's next attempt mistake it for its own
        server.RegisterMethod("prefill", [&writer](const json& params) {
            try {
                writer.Prefill(TurnsFrom(params), {params.value("style", "prose"), "concise"});
            } catch (const std::exception& e) {
                std::fprintf(stderr, "clinicavt-note-host: prefill dropped (%s)\n", e.what());
                ExitIfPoisoned(e.what());
            }
            return json::object();
        });
        server.RegisterMethod("write", [&writer, &lane](const json& params) {
            clinicavt::note::NoteOptions options{params.value("style", "prose"),
                                                 params.value("detail", "concise")};
            options.confirmed = params.value("confirmed", false);
            return lane.Start([&writer, turns = TurnsFrom(params),
                               options = std::move(options)](const auto& progress) {
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

        // The engine is the one client. Losing it ends this serve loop and
        // cancels any generation, so the host exits within seconds. A load
        // cannot be cancelled and finishes first
        server.ServeOneClient();
        writer.Close();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt-note-host: fatal: %s\n", e.what());
        return 1;
    }
}
