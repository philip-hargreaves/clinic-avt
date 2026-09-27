#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <stdexcept>

#include "adapters/demo/sample_year.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/translate/translate_lane.hpp"
#include "core/note/summary_scrub.hpp"

namespace clinicavt::ipc {
json HandleSessionList(clinicavt::store::ISessionStore& sessions) {
    json list = json::array();
    for (const auto& session : sessions.ListSessions()) {
        list.push_back({{"id", session.id},
                        {"startedAt", session.started_at},
                        {"endedAt", session.ended_at},
                        {"label", session.label},
                        {"editedAt", NullWhenEmpty(session.edited_at)},
                        {"audioSeconds", session.audio_seconds},
                        {"demo", session.demo},
                        {"hasReflection", session.has_reflection}});
    }
    return json{{"sessions", std::move(list)}};
}

std::variant<json, Error> HandleDemoSeed(clinicavt::store::ISessionStore& sessions,
                                         const std::filesystem::path& demo_dir) {
    try {
        // Already seeded is a no-op
        if (clinicavt::demo::HasSamples(sessions)) {
            return json{{"added", 0}};
        }
        const auto samples = clinicavt::demo::LoadSampleYear(demo_dir);
        if (samples.empty()) {
            return SessionError("no sample content beside the engine");
        }
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        return json{{"added", clinicavt::demo::SeedSampleYear(sessions, samples, now)}};
    } catch (const std::exception& e) {
        return SessionError(e.what());
    }
}

json HandleDemoClear(clinicavt::store::ISessionStore& sessions) {
    return json{{"removed", sessions.ClearDemo()}};
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
        json result{{"text", patient.text},
                    {"generatedAt", NullWhenEmpty(patient.generated_at)},
                    {"translation", nullptr}};
        if (!translation.text.empty()) {
            result["translation"] =
                json{{"language", translation.language}, {"text", translation.text}};
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

std::variant<json, Error> HandleSessionDeleteAll(clinicavt::store::ISessionStore& sessions,
                                                 bool session_active) {
    if (session_active) return SessionError("finish the consultation first");
    return json{{"removed", sessions.DeleteAll()}};
}

namespace {

constexpr const char* kAnswers[] = {"happened", "learned", "next"};
constexpr const char* kReferenceFields[] = {"key", "reference", "title", "link", "source"};

// A guideline or document the clinician ticked. It keeps its own copy of the words so it
// still reads after the document or the search result is gone
json ReferenceFrom(const json& given) {
    json reference = json::object();
    for (const char* field : kReferenceFields) {
        reference[field] = given.is_object() && given.contains(field) && given[field].is_string()
                               ? given[field]
                               : json("");
    }
    return reference;
}

// The answers and ticked references are one sealed JSON text. Unparseable text reads as empty
json AnswersFrom(const std::string& text) {
    json answers = json::object();
    const json parsed = json::parse(text, nullptr, false);
    for (const char* key : kAnswers) {
        answers[key] = parsed.is_object() && parsed.contains(key) && parsed[key].is_string()
                           ? parsed[key]
                           : json("");
    }
    answers["references"] = json::array();
    if (parsed.is_object() && parsed.contains("references") && parsed["references"].is_array()) {
        for (const auto& given : parsed["references"]) {
            answers["references"].push_back(ReferenceFrom(given));
        }
    }
    return answers;
}

}  // namespace

std::variant<json, Error> HandleReflectionGet(clinicavt::store::ISessionStore& sessions,
                                              const json& params) {
    using clinicavt::store::DocumentKind;
    return WithSession(params, [&](const std::string& session) {
        json result{{"id", session},
                    {"label", sessions.ReadDocument(session, DocumentKind::kLabel).text},
                    {"summary", nullptr},
                    {"reflection", nullptr}};
        // Scrub on read because stored text may predate the scrub or be hand-edited
        const auto summary = sessions.ReadDocument(session, DocumentKind::kSummary);
        if (!summary.text.empty()) {
            result["summary"] = {{"text", clinicavt::note::ScrubSummary(summary.text)},
                                 {"generatedAt", NullWhenEmpty(summary.generated_at)},
                                 {"editedAt", NullWhenEmpty(summary.edited_at)}};
        }
        const auto reflection = sessions.ReadDocument(session, DocumentKind::kReflection);
        if (!reflection.text.empty()) {
            json entry = AnswersFrom(reflection.text);
            entry["createdAt"] = NullWhenEmpty(reflection.generated_at);
            entry["editedAt"] = NullWhenEmpty(reflection.edited_at);
            result["reflection"] = std::move(entry);
        }
        return result;
    });
}

// Given answers and references replace stored ones and omitted ones stay. Only
// reflection/delete removes one
std::variant<json, Error> HandleReflectionUpdate(clinicavt::store::ISessionStore& sessions,
                                                 const json& params) {
    using clinicavt::store::DocumentKind;
    return WithSession(params, [&](const std::string& session) -> std::variant<json, Error> {
        for (const char* key : kAnswers) {
            if (params.contains(key) && !params[key].is_string()) {
                return InvalidParams(std::string(key) + " must be a string");
            }
        }
        if (params.contains("summary") && !params["summary"].is_string()) {
            return InvalidParams("summary must be a string");
        }
        if (params.contains("references")) {
            const auto& references = params["references"];
            const bool objects =
                references.is_array() && std::all_of(references.begin(), references.end(),
                                                     [](const json& r) { return r.is_object(); });
            if (!objects) return InvalidParams("references must be an array of objects");
        }
        if (params.contains("summary")) {
            sessions.EditDocument(
                session, DocumentKind::kSummary,
                clinicavt::note::ScrubSummary(params["summary"].get<std::string>()));
        }
        const auto stored = sessions.ReadDocument(session, DocumentKind::kReflection);
        json answers = AnswersFrom(stored.text);
        for (const char* key : kAnswers) {
            if (params.contains(key)) answers[key] = params[key];
        }
        if (params.contains("references")) {
            answers["references"] = json::array();
            for (const auto& given : params["references"]) {
                answers["references"].push_back(ReferenceFrom(given));
            }
        }
        if (stored.text.empty()) {
            clinicavt::store::Document document;
            document.text = answers.dump();
            sessions.SaveDocument(session, DocumentKind::kReflection, document);
        } else {
            sessions.EditDocument(session, DocumentKind::kReflection, answers.dump());
        }
        return json::object();
    });
}

std::variant<json, Error> HandleReflectionDelete(clinicavt::store::ISessionStore& sessions,
                                                 const json& params) {
    using clinicavt::store::DocumentKind;
    return WithSession(params, [&](const std::string& id) {
        sessions.DeleteDocument(id, DocumentKind::kReflection);
        sessions.DeleteDocument(id, DocumentKind::kSummary);
        return json::object();
    });
}

// Every session with an appraisal entry, newest first
json HandleReflectionList(clinicavt::store::ISessionStore& sessions) {
    using clinicavt::store::DocumentKind;
    json list = json::array();
    for (const auto& session : sessions.ListSessions()) {
        if (!session.has_reflection) continue;
        try {
            const auto reflection = sessions.ReadDocument(session.id, DocumentKind::kReflection);
            const auto summary = sessions.ReadDocument(session.id, DocumentKind::kSummary);
            const json answers = AnswersFrom(reflection.text);
            list.push_back({{"id", session.id},
                            {"startedAt", session.started_at},
                            {"label", session.label},
                            {"happened", answers["happened"]},
                            {"learned", answers["learned"]},
                            {"next", answers["next"]},
                            {"summary", clinicavt::note::ScrubSummary(summary.text)},
                            {"createdAt", NullWhenEmpty(reflection.generated_at.empty()
                                                            ? summary.generated_at
                                                            : reflection.generated_at)},
                            {"editedAt", NullWhenEmpty(reflection.edited_at)},
                            {"demo", session.demo}});
        } catch (const std::exception&) {
            // A session mid-recording or missing its key is not listed
        }
    }
    return json{{"reflections", std::move(list)}};
}

namespace {

// The handler for a text edit of one stored document
auto EditDocument(clinicavt::store::ISessionStore& sessions, clinicavt::store::DocumentKind kind) {
    return [&sessions, kind](const json& params) {
        return WithSession(params, [&](const std::string& id) -> std::variant<json, Error> {
            if (!params.contains("text") || !params["text"].is_string()) {
                return InvalidParams("text must be a string");
            }
            sessions.EditDocument(id, kind, params["text"].get<std::string>());
            return json::object();
        });
    };
}

// Checks style and detail as the shell sends them. confirmed says the
// clinician insists it was a consultation
std::variant<clinicavt::note::NoteOptions, Error> NoteOptionsFrom(const json& params) {
    const std::string style = params.value("style", "prose");
    const std::string detail = params.value("detail", "standard");
    if (style != "prose" && style != "soap") return InvalidParams("unknown style: " + style);
    if (detail != "concise" && detail != "standard" && detail != "detailed") {
        return InvalidParams("unknown detail: " + detail);
    }
    clinicavt::note::NoteOptions options{style, detail};
    options.confirmed = params.value("confirmed", false);
    return options;
}

}  // namespace

void RegisterSessionMethods(PipeServer& server, const EngineServices& services) {
    auto& controller = services.controller;
    auto& sessions = services.sessions;
    auto* const translator = services.translator;
    auto* const translate_lane = services.translate_lane;
    const auto demo_dir = services.demo_dir;
    auto* const playback = services.playback;
    server.RegisterMethod("session/list",
                          [&sessions](const json&) { return HandleSessionList(sessions); });
    server.RegisterMethod("session/transcript", [&sessions](const json& params) {
        return HandleSessionTranscript(sessions, params);
    });
    server.RegisterMethod("session/note", [&sessions](const json& params) {
        return HandleSessionNote(sessions, params);
    });
    server.RegisterMethod("session/patient", [&sessions](const json& params) {
        return HandleSessionPatient(sessions, params);
    });
    // A typed label outlives regenerations. The note's own first sentence
    // fills in until then
    server.RegisterMethod("session/label",
                          EditDocument(sessions, clinicavt::store::DocumentKind::kLabel));
    if (translator != nullptr && translate_lane != nullptr) {
        server.RegisterMethod("translate/languages", [translator](const json&) {
            return json{{"languages", translator->Languages()}};
        });
        // Translates the session's patient sheet off the RPC thread. Results
        // arrive as translate/partial then translate/ready
        server.RegisterMethod("patient/translate", [&sessions, translate_lane](const json& params) {
            return WithSession(
                params, [&](const std::string& session_id) -> std::variant<json, Error> {
                    if (!params.contains("language") || !params["language"].is_string()) {
                        return InvalidParams("language must be a string");
                    }
                    const auto text =
                        sessions.ReadDocument(session_id, clinicavt::store::DocumentKind::kPatient)
                            .text;
                    if (text.empty()) {
                        return SessionError("no patient information to translate");
                    }
                    // Stored before translate/ready goes out, so the sheet
                    // read back after it already carries the translation
                    const auto on_ready = [&sessions, session_id](const std::string& translated,
                                                                  const std::string& language) {
                        try {
                            clinicavt::store::Document document;
                            document.text = translated;
                            document.language = language;
                            sessions.SaveDocument(
                                session_id, clinicavt::store::DocumentKind::kTranslation, document);
                        } catch (...) {  // NOLINT(bugprone-empty-catch)
                        }
                    };
                    if (!translate_lane->Run(text, params["language"].get<std::string>(),
                                             on_ready)) {
                        return SessionError("a translation is already running");
                    }
                    return json::object();
                });
        });
    }
    server.RegisterMethod("session/delete", [&sessions](const json& params) {
        return HandleSessionDelete(sessions, params);
    });
    // The shell confirms first
    server.RegisterMethod("session/deleteAll", [&sessions, &controller](const json&) {
        return HandleSessionDeleteAll(sessions, controller.Running());
    });
    server.RegisterMethod("reflection/get", [&sessions](const json& params) {
        return HandleReflectionGet(sessions, params);
    });
    server.RegisterMethod("reflection/update", [&sessions](const json& params) {
        return HandleReflectionUpdate(sessions, params);
    });
    server.RegisterMethod("reflection/delete", [&sessions](const json& params) {
        return HandleReflectionDelete(sessions, params);
    });
    server.RegisterMethod("reflection/list",
                          [&sessions](const json&) { return HandleReflectionList(sessions); });
    server.RegisterMethod("demo/seed", [&sessions, demo_dir](const json&) {
        return HandleDemoSeed(sessions, demo_dir);
    });
    server.RegisterMethod("demo/clear",
                          [&sessions](const json&) { return HandleDemoClear(sessions); });
    // Written on the note lane and delivered as reflection/summary
    server.RegisterMethod(
        "reflection/summary", [&controller](const json& params) -> std::variant<json, Error> {
            const auto id = IdFrom(params);
            if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
            if (!controller.WriteSummary(std::get<std::string>(id))) {
                return SessionError("no stored note, or a document is already being written");
            }
            return json::object();
        });
    server.RegisterMethod(
        "session/start",
        [&controller, playback, translator](const json& params) -> std::variant<json, Error> {
            // A playback block replays a stored consultation as a demo.
            // Nothing is captured or generated
            if (params.contains("playback")) {
                const auto& p = params["playback"];
                if (playback == nullptr || !p.contains("id") || !p["id"].is_string()) {
                    return Error{kInvalidParams, "playback.id is required", {}};
                }
                if (controller.Running() || !playback->Start(p["id"].get<std::string>())) {
                    return SessionError("a session is running, or nothing to play back");
                }
                return json{{"sessionId", playback->Current()}};
            }
            if (playback != nullptr && playback->Active()) {
                return SessionError("a playback is running");
            }
            // An optional replay block plays a file through the same
            // pipeline. Absent means microphone
            std::optional<clinicavt::session::ReplaySpec> replay;
            if (params.contains("replay")) {
                const auto& r = params["replay"];
                if (!r.contains("path") || !r["path"].is_string()) {
                    return Error{kInvalidParams, "replay.path is required", {}};
                }
                replay = clinicavt::session::ReplaySpec{
                    r["path"].get<std::string>(), r.value("speed", 1.0), r.value("monitor", false)};
            }
            // micId pins the picker's choice. One that has gone falls back
            // to the default, logged, and the snapshot records the fallback
            clinicavt::session::MicSelection mic;
            if (!replay.has_value()) {
                const std::string requested = params.value("micId", "");
                const auto device = clinicavt::audio::ResolveMicrophone(
                    clinicavt::audio::ListCaptureDevices(), requested);
                if (!requested.empty() && device.id != requested) {
                    std::fprintf(stderr, "clinicavt-engine: chosen microphone gone, using %s\n",
                                 device.name.empty() ? "the default" : device.name.c_str());
                }
                mic = {device.id, device.name};
            }
            // resume replays the crashed session's stored audio ahead of the live
            // source. retain false erases on leaving the consultation
            if (!controller.Start(std::move(replay), params.value("resume", ""),
                                  params.value("retain", true), mic)) {
                return Error{kCaptureFailed, "Capture failed", json(controller.LastEnd().detail)};
            }
            // Capture and the note need the memory
            if (translator != nullptr) translator->Release();
            return json{{"sessionId", controller.CurrentSession()}};
        });
    server.RegisterMethod(
        "note/options", [&controller](const json& params) -> std::variant<json, Error> {
            const auto options = NoteOptionsFrom(params);
            if (std::holds_alternative<Error>(options)) return std::get<Error>(options);
            controller.SetNoteOptions(std::get<clinicavt::note::NoteOptions>(options));
            return json::object();
        });
    server.RegisterMethod(
        "note/regenerate", [&controller](const json& params) -> std::variant<json, Error> {
            const auto options = NoteOptionsFrom(params);
            if (std::holds_alternative<Error>(options)) return std::get<Error>(options);
            if (!controller.RegenerateNote(std::get<clinicavt::note::NoteOptions>(options))) {
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
    server.RegisterMethod("session/pause", [&controller, playback](const json& params) {
        if (playback != nullptr && playback->Listening()) {
            playback->SetPaused(params.value("paused", true));
        } else {
            controller.SetPaused(params.value("paused", true));
        }
        return json::object();
    });
    server.RegisterMethod("session/monitor", [&controller](const json& params) {
        controller.SetMonitor(params.value("on", true));
        return json::object();
    });
    server.RegisterMethod("session/cancel", [&controller, playback](const json&) {
        if (playback != nullptr && playback->Listening()) {
            playback->Cancel();
        } else {
            controller.Cancel();
        }
        return json::object();
    });
    // Regenerate and translate act on a past session under review as on a
    // fresh seal. Record closes the review. A stored sheet preloads the
    // translator
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
    // The note and patient lanes announce themselves when a writer is
    // wired. Without one the stubs keep the contract for CI
    server.RegisterMethod("session/stop", [&server, &controller, playback](const json&) {
        if (playback != nullptr && playback->Active()) {
            playback->Stop();
            return json{{"sessionId", playback->Current()}};
        }
        controller.Stop();
        if (!controller.HasNoteWriter()) {
            server.QueueNotification("note/ready", json::object());
            server.QueueNotification("patient/ready", json::object());
        }
        return json{{"sessionId", controller.LastFinalised()}};
    });
}

}  // namespace clinicavt::ipc
