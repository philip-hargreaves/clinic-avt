#include <memory>
#include <openvino/core/version.hpp>
#include <optional>
#include <stdexcept>

#include "adapters/ipc/handlers.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/system/power_throttling.hpp"
#include "core/common/version.hpp"

namespace clinicavt::ipc {

std::variant<json, Error> HandleHello(const json& params) {
    const auto peer = PeerInfoFromJson(params);
    if (!peer) {
        return InvalidParams("expected name, version, protocolVersion");
    }
    return ToJson(PeerInfo{clinicavt::kName, clinicavt::kVersion, kProtocolVersion});
}

std::variant<json, Error> HandleEcho(const json& params) {
    if (!params.contains("payload") || !params["payload"].is_string()) {
        return InvalidParams("expected payload string");
    }
    return json{{"payload", params["payload"]}};
}

json HandleAudioInputs(const std::vector<clinicavt::audio::CaptureDevice>& devices) {
    json list = json::array();
    for (const auto& device : devices) {
        list.push_back({{"id", device.id},
                        {"name", device.name},
                        {"shortName", device.short_name},
                        {"isDefault", device.is_default},
                        {"bluetooth", device.bluetooth}});
    }
    return json{{"devices", std::move(list)}};
}

json HandleAnchorStatus(const clinicavt::diar::AnchorStore& anchors) {
    const auto status = anchors.Status();
    const char* origin = status.origin == clinicavt::diar::AnchorOrigin::kEnrolled  ? "enrolled"
                         : status.origin == clinicavt::diar::AnchorOrigin::kAccrued ? "accrued"
                                                                                    : "none";
    json result{{"origin", origin}, {"sessions", status.sessions}};
    result["enrolledAt"] = status.enrolled_at == 0 ? json(nullptr) : json(status.enrolled_at);
    return result;
}

std::variant<json, Error> HandleAnchorClear(clinicavt::diar::AnchorStore& anchors,
                                            bool session_active) {
    if (session_active) {
        return SessionError("finish the consultation first");
    }
    anchors.Clear();
    return json::object();
}

json HandleModels(const clinicavt::models::ModelStore& models, const std::string& note_tier) {
    json list = json::array();
    for (const auto& model : models.List()) {
        const bool active = model.tier == (model.task == "note" ? note_tier : "default");
        list.push_back({{"id", model.id},
                        {"name", model.name},
                        {"task", model.task},
                        {"tier", model.tier},
                        {"device", model.device},
                        {"licence", model.licence},
                        {"active", active}});
    }
    return json{{"models", std::move(list)}};
}

namespace {

const char* PhaseName(clinicavt::note::NoteModelState::Phase phase) {
    switch (phase) {
        case clinicavt::note::NoteModelState::Phase::kLoading:
            return "loading";
        case clinicavt::note::NoteModelState::Phase::kReady:
            return "ready";
        case clinicavt::note::NoteModelState::Phase::kFailed:
            return "failed";
        default:
            return "idle";
    }
}

}  // namespace

json NoteModelJson(const clinicavt::note::NoteModelState& state) {
    json result{{"tier", state.tier},
                {"id", state.id},
                {"name", state.name},
                {"state", PhaseName(state.phase)}};
    if (state.phase == clinicavt::note::NoteModelState::Phase::kLoading) {
        result["firstUse"] = state.first_use;
    }
    if (state.phase == clinicavt::note::NoteModelState::Phase::kReady) {
        result["seconds"] = state.seconds;
    }
    if (state.phase == clinicavt::note::NoteModelState::Phase::kFailed) {
        result["detail"] = state.detail;
    }
    return result;
}

std::variant<json, Error> HandleNoteTier(clinicavt::note::INoteLane* lane, bool session_active,
                                         const json& params) {
    if (!params.contains("tier") || !params["tier"].is_string()) {
        return InvalidParams("tier must be a string");
    }
    const std::string tier = params["tier"].get<std::string>();
    if (tier != "default" && tier != "accuracy" && tier != "constrained") {
        return InvalidParams("unknown tier: " + tier);
    }
    if (lane == nullptr) {
        return SessionError("no note model is staged");
    }
    if (session_active) {
        return SessionError("finish the consultation before changing the note model");
    }
    try {
        return NoteModelJson(lane->Configure(tier));
    } catch (const std::invalid_argument& e) {
        return InvalidParams(e.what());
    } catch (const std::exception& e) {
        return SessionError(e.what());
    }
}

std::variant<json, Error> HandleAsrDevice(const AsrSwitch& switcher, bool session_active,
                                          const json& params, std::function<void(json)> notify) {
    const std::string device = params.value("device", "");
    if (device != "GPU" && device != "NPU") {
        return InvalidParams("device must be GPU or NPU");
    }
    if (session_active) {
        return SessionError("finish the consultation before switching transcription device");
    }
    const bool started =
        switcher && switcher(device, [device, notify](const std::string& error) {
            json state{{"device", device}, {"state", error.empty() ? "ready" : "failed"}};
            if (!error.empty()) state["detail"] = error;
            notify(std::move(state));
        });
    if (!started) {
        return SessionError("speech recognition cannot move to another device here");
    }
    return json{{"device", device}, {"state", "loading"}};
}

void RegisterEngineMethods(PipeServer& server, const EngineServices& services) {
    auto& controller = services.controller;
    const auto& models = services.models;
    auto* const metrics = services.metrics;
    auto* const runtime = services.runtime;
    const bool first_use = services.first_use;
    auto* const anchors = services.anchors;
    auto* const note_lane = services.note_lane;
    const bool stray_note_host = services.stray_note_host;
    server.RegisterMethod("engine/hello", HandleHello);
    server.RegisterMethod("engine/echo", HandleEcho);
    const auto note_tier = [note_lane] {
        return note_lane != nullptr ? note_lane->State().tier : std::string("default");
    };
    // Ready when every staged model's compile cache exists. OpenVINO writes
    // the blob exactly when a compile completes, so no event plumbing is needed.
    // strayNoteHost: a note host from an earlier engine is still alive, wedged
    // in the GPU driver. Only a reboot ends it
    server.RegisterMethod(
        "engine/readiness", [&models, first_use, note_tier, stray_note_host](const json&) {
            const auto ready = [&models](const char* role, const std::string& tier) {
                try {
                    const auto cache = clinicavt::models::CacheDir(models.Resolve(role, tier));
                    return std::filesystem::exists(cache) && !std::filesystem::is_empty(cache);
                } catch (...) {
                    return true;  // role not staged: nothing to wait for
                }
            };
            return json{{"firstUse", first_use},
                        {"ready", ready("asr", "default") && ready("note", note_tier()) &&
                                      ready("translation", "default")},
                        {"strayNoteHost", stray_note_host}};
        });
    server.RegisterMethod("note/tier", [note_lane, &controller](const json& params) {
        return HandleNoteTier(note_lane, controller.Busy(), params);
    });
    server.RegisterMethod(
        "asr/device", [switcher = services.switch_asr, &controller, &server](const json& params) {
            return HandleAsrDevice(switcher, controller.Running(), params, [&server](json state) {
                server.PushNotification("asr/device", std::move(state));
            });
        });
    if (metrics != nullptr) {
        // Device names are enumerated once, on the first fetch
        auto hardware = std::make_shared<std::optional<json>>();
        server.RegisterMethod("engine/metrics", [metrics, runtime, hardware](const json&) {
            if (!hardware->has_value()) {
                *hardware = runtime != nullptr ? json(runtime->DescribeDevices()) : json::object();
            }
            const auto s = metrics->Take();
            return json{
                {"devices", s.devices},
                {"loadSeconds", s.load_seconds},
                {"stageSeconds", s.stage_seconds},
                {"asrRealtimeFactor",
                 s.decode_busy_seconds > 0 ? s.decoded_audio_seconds / s.decode_busy_seconds : 0},
                {"audioSeconds", s.session_audio_seconds},
                {"lostFrames", s.lost_frames},
                {"diarTicks", s.diar_ticks},
                {"turns", s.turns},
                {"clusters", s.clusters},
                {"replay", s.replay},
                {"replaySpeed", s.replay_speed},
                {"hardware", **hardware},
                {"openvino", std::string(ov::get_openvino_version().buildNumber)},
                {"powerThrottling", system::Describe(system::ReadThrottling(GetCurrentProcess()))}};
        });
    }
    server.RegisterMethod("engine/models", [&models, note_tier](const json&) {
        return HandleModels(models, note_tier());
    });
    if (anchors != nullptr) {
        server.RegisterMethod("anchor/status",
                              [anchors](const json&) { return HandleAnchorStatus(*anchors); });
        server.RegisterMethod("anchor/clear", [anchors, &controller](const json&) {
            return HandleAnchorClear(*anchors, controller.Running());
        });
        // Enrolment runs on the controller's microphone path. Progress and the
        // outcome are notifications
        server.RegisterMethod(
            "anchor/enrol", [&controller](const json& params) -> std::variant<json, Error> {
                const double seconds = params.value("seconds", 45.0);
                clinicavt::session::MicSelection mic;
                if (params.contains("mic") && params["mic"].is_object()) {
                    mic.id = params["mic"].value("id", "");
                }
                if (seconds <= 0 || seconds > 300) {
                    return InvalidParams("seconds must be 1-300");
                }
                if (!controller.StartEnrolment(seconds, mic)) {
                    return SessionError("a consultation or an enrolment is already running");
                }
                return json::object();
            });
        server.RegisterMethod("anchor/enrol/cancel", [&controller](const json&) {
            controller.CancelEnrolment();
            return json::object();
        });
        server.RegisterMethod("anchor/enrol/finish", [&controller](const json&) {
            controller.FinishEnrolment();
            return json::object();
        });
    }
    // Enumerated fresh per call, so a picker opened after a headset is
    // plugged in sees it without any notification plumbing
    server.RegisterMethod("audio/inputs", [](const json&) {
        return HandleAudioInputs(clinicavt::audio::ListCaptureDevices());
    });
}

}  // namespace clinicavt::ipc
