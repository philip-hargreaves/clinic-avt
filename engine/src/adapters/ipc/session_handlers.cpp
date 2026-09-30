#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "adapters/archive/archive_lane.hpp"
#include "adapters/interfaces/translator.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/ipc/session_common.hpp"
#include "core/common/log.hpp"
#include "core/common/strings.hpp"

namespace clinicavt::ipc {

json HandleSessionList(clinicavt::records::SessionRecords& records) {
    json list = json::array();
    for (const auto& session : records.Consultations()) {
        list.push_back({{"id", session.id},
                        {"startedAt", session.started_at},
                        {"endedAt", session.ended_at},
                        {"label", session.label},
                        {"editedAt", NullWhenEmpty(session.edited_at)},
                        {"audioSeconds", session.audio_seconds},
                        {"sample", session.sample},
                        {"hasReflection", session.has_reflection}});
    }
    return json{{"sessions", std::move(list)}};
}

std::variant<json, Error> HandleSessionTranscript(clinicavt::store::ISessionStore& sessions,
                                                  const json& params) {
    return WithSession(params, [&](const std::string& id) {
        return json{{"turns", TurnsJson(sessions.ReadTurns(id))}};
    });
}

std::variant<json, Error> HandleSessionNote(clinicavt::store::ISessionStore& sessions,
                                            const json& params) {
    return WithSession(params, [&](const std::string& id) {
        const auto note = sessions.ReadDocument(id, clinicavt::store::DocumentKind::kNote);
        return json{{"text", note.text},
                    {"style", note.style},
                    {"detail", note.detail},
                    {"generatedAt", NullWhenEmpty(note.generated_at)},
                    {"editedAt", NullWhenEmpty(note.edited_at)}};
    });
}

std::variant<json, Error> HandleSessionPatient(clinicavt::store::ISessionStore& sessions,
                                               const json& params) {
    return WithSession(params, [&](const std::string& id) {
        using clinicavt::store::DocumentKind;
        const auto patient = sessions.ReadDocument(id, DocumentKind::kPatient);
        const auto translation = sessions.ReadDocument(id, DocumentKind::kTranslation);
        // Both timestamps so the shell can flag a sheet edited after translation
        json result{{"text", patient.text},
                    {"generatedAt", NullWhenEmpty(patient.generated_at)},
                    {"editedAt", NullWhenEmpty(patient.edited_at)},
                    {"translation", nullptr}};
        if (!translation.text.empty()) {
            result["translation"] = json{{"language", translation.language},
                                         {"text", translation.text},
                                         {"translatedAt", NullWhenEmpty(translation.generated_at)}};
        }
        return result;
    });
}

std::variant<json, Error> HandleSessionDelete(clinicavt::store::ISessionStore& sessions,
                                              const json& params) {
    return WithSession(params, [&](const std::string& id) {
        sessions.Delete(id);
        return json::object();
    });
}

namespace {

// deleteReflections false or absent keeps each appraisal entry and its case summary
std::optional<bool> DeleteReflections(const json& params) {
    if (!params.is_object() || !params.contains("deleteReflections")) return false;
    if (!params["deleteReflections"].is_boolean()) return std::nullopt;
    return params["deleteReflections"].get<bool>();
}

}  // namespace

std::variant<json, Error> HandleSessionDeleteAll(clinicavt::records::SessionRecords& records,
                                                 const json& params, bool session_active,
                                                 bool archive_busy) {
    const auto delete_reflections = DeleteReflections(params);
    if (!delete_reflections) return InvalidParams("deleteReflections must be true or false");
    if (session_active) return SessionError("finish the consultation first");
    if (archive_busy) return SessionError(kArchiveRunning);
    try {
        return json{{"removed", records.DeleteAll(!*delete_reflections)}};
    } catch (const std::exception& e) {
        return SessionError(e.what());
    }
}

std::variant<json, Error> HandleSessionRemove(clinicavt::records::SessionRecords& records,
                                              const json& params, bool archive_busy) {
    const auto delete_reflections = DeleteReflections(params);
    if (!delete_reflections) return InvalidParams("deleteReflections must be true or false");
    const bool listed = params.is_object() && params.contains("ids") && params["ids"].is_array() &&
                        std::all_of(params["ids"].begin(), params["ids"].end(),
                                    [](const json& id) { return id.is_string(); });
    if (!listed) return InvalidParams("ids must be a list of session ids");
    if (archive_busy) return SessionError(kArchiveRunning);
    std::vector<clinicavt::store::SessionId> ids;
    for (const auto& id : params["ids"]) ids.push_back(id.get<std::string>());
    try {
        return json{{"removed", records.Remove(ids, !*delete_reflections)}};
    } catch (const std::exception& e) {
        return SessionError(e.what());
    }
}

namespace {

auto EditDocument(clinicavt::store::ISessionStore& sessions, clinicavt::store::DocumentKind kind) {
    return [&sessions, kind](const json& params) {
        return WithSession(params, [&](const std::string& id) -> std::variant<json, Error> {
            if (!params.contains("text") || !params["text"].is_string()) {
                return InvalidParams("text must be a string");
            }
            sessions.EditDocument(id, kind,
                                  clinicavt::strings::UnixLines(params["text"].get<std::string>()));
            return json::object();
        });
    };
}

// A stored session with no turns, e.g. restored without a transcript
bool NoTranscript(clinicavt::store::ISessionStore& sessions, const std::string& id) {
    if (id.empty()) return false;
    try {
        return sessions.ReadTurns(id).empty();
    } catch (const std::exception&) {
        return false;
    }
}

// confirmed: clinician says it was a consultation. Legacy detail "standard" maps to concise
std::variant<clinicavt::note::NoteOptions, Error> NoteOptionsFrom(const json& params) {
    const std::string style = params.value("style", "prose");
    std::string detail = params.value("detail", "concise");
    if (detail == "standard") detail = "concise";
    if (style != "prose" && style != "soap") return InvalidParams("unknown style: " + style);
    if (detail != "concise" && detail != "detailed") {
        return InvalidParams("unknown detail: " + detail);
    }
    clinicavt::note::NoteOptions options{*clinicavt::note::NoteStyleFrom(style),
                                         *clinicavt::note::NoteDetailFrom(detail)};
    options.confirmed = params.value("confirmed", false);
    return options;
}

Notify QueueTo(PipeServer& server) {
    return [&server](const std::string& method, json params) {
        server.QueueNotification(method, std::move(params));
    };
}

}  // namespace

std::function<bool()> ArchiveBusy(const EngineServices& services) {
    auto* const archive_lane = services.archive_lane;
    return [archive_lane] { return archive_lane != nullptr && archive_lane->Busy(); };
}

void StubDocuments(const Notify& notify) {
    notify("note/ready", json::object());
    notify("patient/ready", json::object());
}

std::variant<json, Error> HandleSessionStart(clinicavt::session::SessionController& controller,
                                             clinicavt::translate::ITranslator* translator,
                                             const json& params,
                                             const std::vector<std::string>& missing,
                                             bool allow_replay) {
    // replay plays a file through the live pipeline in place of the microphone
    std::optional<clinicavt::session::ReplaySpec> replay;
    if (params.contains("replay")) {
        // Replay reads any file the client names, so only an engine started with
        // --allow-replay accepts it
        if (!allow_replay) {
            return InvalidParams("replay needs an engine started with --allow-replay");
        }
        const auto& r = params["replay"];
        if (!r.contains("path") || !r["path"].is_string()) {
            return InvalidParams("replay.path is required");
        }
        replay =
            clinicavt::session::ReplaySpec{r["path"].get<std::string>(), r.value("speed", 1.0)};
    }
    if (!missing.empty()) return SessionError(MissingModelsReason(missing));
    // micId pins the picker choice. A missing device falls back to the default,
    // logged and recorded in the snapshot
    clinicavt::session::MicSelection mic;
    if (!replay.has_value()) {
        const std::string requested = params.value("micId", "");
        const auto device =
            clinicavt::audio::ResolveMicrophone(clinicavt::audio::ListCaptureDevices(), requested);
        if (!requested.empty() && device.id != requested) {
            log::Printf("clinicavt-engine: chosen microphone gone, using %s\n",
                        device.name.empty() ? "the default" : device.name.c_str());
        }
        mic = {device.id, device.name};
    }
    // resume: replay the crashed session's stored audio before the live source.
    // retain false: erase when the consultation is left
    if (!controller.Start(std::move(replay), params.value("resume", ""),
                          params.value("retain", true), mic)) {
        return Error{kCaptureFailed, "Capture failed", json(controller.LastEnd().detail)};
    }
    // Capture and the note need the memory
    if (translator != nullptr) translator->Release();
    return json{{"sessionId", controller.CurrentSession()}};
}

void RegisterSessionMethods(PipeServer& server, const EngineServices& services) {
    auto& controller = services.controller;
    auto& sessions = services.sessions;
    auto& records = services.records;
    auto* const translator = services.translator;
    const auto archive_busy = ArchiveBusy(services);
    server.RegisterMethod("session/list",
                          [&records](const json&) { return HandleSessionList(records); });
    server.RegisterMethod("session/transcript", [&sessions](const json& params) {
        return HandleSessionTranscript(sessions, params);
    });
    server.RegisterMethod("session/note", [&sessions](const json& params) {
        return HandleSessionNote(sessions, params);
    });
    server.RegisterMethod("session/patient", [&sessions](const json& params) {
        return HandleSessionPatient(sessions, params);
    });
    // A typed label survives regeneration. Until one is set, the note's first sentence is used
    server.RegisterMethod("session/label",
                          EditDocument(sessions, clinicavt::store::DocumentKind::kLabel));
    server.RegisterMethod(
        "session/delete",
        [&sessions, &controller, archive_busy](const json& params) -> std::variant<json, Error> {
            if (archive_busy()) return SessionError(kArchiveRunning);
            if (controller.Busy()) return SessionError("finish the consultation first");
            return HandleSessionDelete(sessions, params);
        });
    // The shell confirms first
    server.RegisterMethod(
        "session/deleteAll", [&records, &controller, archive_busy](const json& params) {
            return HandleSessionDeleteAll(records, params, controller.Busy(), archive_busy());
        });
    server.RegisterMethod(
        "session/remove",
        [&records, &controller, archive_busy](const json& params) -> std::variant<json, Error> {
            if (controller.Busy()) return SessionError("finish the consultation first");
            return HandleSessionRemove(records, params, archive_busy());
        });
    server.RegisterMethod(
        "session/start", [&controller, translator, missing = services.missing_models,
                          allow_replay = services.allow_replay](const json& params) {
            return HandleSessionStart(controller, translator, params, missing, allow_replay);
        });
    server.RegisterMethod(
        "note/options", [&controller](const json& params) -> std::variant<json, Error> {
            const auto options = NoteOptionsFrom(params);
            if (std::holds_alternative<Error>(options)) return std::get<Error>(options);
            controller.SetNoteOptions(std::get<clinicavt::note::NoteOptions>(options));
            return json::object();
        });
    server.RegisterMethod(
        "note/regenerate",
        [&controller, &sessions](const json& params) -> std::variant<json, Error> {
            const auto options = NoteOptionsFrom(params);
            if (std::holds_alternative<Error>(options)) return std::get<Error>(options);
            if (!controller.RegenerateNote(std::get<clinicavt::note::NoteOptions>(options))) {
                if (NoTranscript(sessions, controller.LastFinalised())) {
                    return SessionError("this consultation has no transcript to write a note from");
                }
                return SessionError("no finalised session, or a note is already being written");
            }
            return json::object();
        });
    // Rewrites the sheet from the stored note, edits included, and streams as usual
    server.RegisterMethod(
        "patient/regenerate", [&controller](const json&) -> std::variant<json, Error> {
            if (!controller.RegeneratePatient()) {
                return SessionError("no stored note, or a document is already being written");
            }
            return json::object();
        });
    server.RegisterMethod("note/update",
                          EditDocument(sessions, clinicavt::store::DocumentKind::kNote));
    server.RegisterMethod("patient/update",
                          EditDocument(sessions, clinicavt::store::DocumentKind::kPatient));
    server.RegisterMethod("session/cancel", [&controller](const json&) {
        controller.Cancel();
        return json::object();
    });
    // Regenerate and translate work on a reviewed past session as on a new one.
    // Recording closes the review. A stored sheet preloads the translator
    server.RegisterMethod(
        "session/open",
        [&controller, &sessions, translator](const json& params) -> std::variant<json, Error> {
            const auto id = IdFrom(params);
            if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
            if (!controller.Open(std::get<std::string>(id))) {
                return SessionError("recording, a note is being written, or no such session");
            }
            if (translator != nullptr) {
                try {
                    const auto sheet = sessions.ReadDocument(
                        std::get<std::string>(id), clinicavt::store::DocumentKind::kPatient);
                    if (!sheet.text.empty()) translator->Prepare();
                } catch (...) {  // NOLINT(bugprone-empty-catch) Translate loads it anyway
                }
            }
            return json::object();
        });
    server.RegisterMethod("session/close", [&controller](const json&) {
        controller.Close();
        return json::object();
    });
    server.RegisterMethod("session/stop",
                          [&server, &controller](const json&) -> std::variant<json, Error> {
                              // Stop would block on the import and hold up every other request
                              if (controller.Importing())
                                  return SessionError("an import is running");
                              controller.Stop();
                              if (!controller.HasNoteWriter()) StubDocuments(QueueTo(server));
                              return json{{"sessionId", controller.LastFinalised()}};
                          });
}

}  // namespace clinicavt::ipc
