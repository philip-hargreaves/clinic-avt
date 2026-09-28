#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "adapters/archive/archive_lane.hpp"
#include "adapters/demo/sample_year.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/translate/translate_lane.hpp"
#include "core/archive/record_rules.hpp"
#include "core/common/iso8601.hpp"
#include "core/common/strings.hpp"
#include "core/common/utf8.hpp"
#include "core/note/summary_scrub.hpp"

namespace clinicavt::ipc {
// A cleared session is an appraisal entry, listed by reflection/list alone
json HandleSessionList(clinicavt::store::ISessionStore& sessions) {
    json list = json::array();
    for (const auto& session : sessions.ListSessions()) {
        if (session.cleared) continue;
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
        // Both times, so the shell can flag a sheet edited after its translation
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

constexpr const char* kArchiveRunning = "a backup or restore is running";

// deleteReflections: false, or absent, keeps each appraisal entry with its case summary
std::optional<bool> DeleteReflections(const json& params) {
    if (!params.is_object() || !params.contains("deleteReflections")) return false;
    if (!params["deleteReflections"].is_boolean()) return std::nullopt;
    return params["deleteReflections"].get<bool>();
}

}  // namespace

std::variant<json, Error> HandleSessionDeleteAll(clinicavt::store::ISessionStore& sessions,
                                                 const json& params, bool session_active,
                                                 bool archive_busy) {
    const auto delete_reflections = DeleteReflections(params);
    if (!delete_reflections) return InvalidParams("deleteReflections must be true or false");
    if (session_active) return SessionError("finish the consultation first");
    if (archive_busy) return SessionError(kArchiveRunning);
    try {
        return json{{"removed", sessions.DeleteAll(!*delete_reflections)}};
    } catch (const std::exception& e) {
        return SessionError(e.what());
    }
}

std::variant<json, Error> HandleSessionRemove(clinicavt::store::ISessionStore& sessions,
                                              const json& params, bool archive_busy) {
    const auto delete_reflections = DeleteReflections(params);
    if (!delete_reflections) return InvalidParams("deleteReflections must be true or false");
    const bool listed = params.is_object() && params.contains("ids") && params["ids"].is_array() &&
                        std::all_of(params["ids"].begin(), params["ids"].end(),
                                    [](const json& id) { return id.is_string(); });
    if (!listed) return InvalidParams("ids must be a list of session ids");
    if (archive_busy) return SessionError(kArchiveRunning);
    std::size_t removed = 0;
    for (const auto& id : params["ids"]) {
        try {
            if (*delete_reflections) {
                sessions.Delete(id.get<std::string>());
            } else {
                sessions.Clear(id.get<std::string>());
            }
            removed += 1;
        } catch (const clinicavt::store::StoreError& e) {
            if (e.Code() != clinicavt::store::StoreCode::kNotFound) return SessionError(e.what());
        } catch (const std::exception& e) {
            return SessionError(e.what());
        }
    }
    return json{{"removed", removed}};
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
            sessions.EditDocument(session, DocumentKind::kSummary,
                                  clinicavt::note::ScrubSummary(clinicavt::strings::UnixLines(
                                      params["summary"].get<std::string>())));
        }
        const auto stored = sessions.ReadDocument(session, DocumentKind::kReflection);
        json answers = AnswersFrom(stored.text);
        for (const char* key : kAnswers) {
            if (params.contains(key)) {
                answers[key] = clinicavt::strings::UnixLines(params[key].get<std::string>());
            }
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
        // A cleared consultation was kept only for its appraisal entry, so it goes with it
        if (sessions.ReadTurns(id).empty() &&
            sessions.ReadDocument(id, DocumentKind::kNote).revision == 0) {
            sessions.Delete(id);
        }
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
            sessions.EditDocument(id, kind,
                                  clinicavt::strings::UnixLines(params["text"].get<std::string>()));
            return json::object();
        });
    };
}

// A stored session whose transcript has no turns, as one restored without it
bool NoTranscript(clinicavt::store::ISessionStore& sessions, const std::string& id) {
    if (id.empty()) return false;
    try {
        return sessions.ReadTurns(id).empty();
    } catch (const std::exception&) {
        return false;
    }
}

// confirmed: the clinician insists it was a consultation. "standard", the retired middle
// length, reads as concise
std::variant<clinicavt::note::NoteOptions, Error> NoteOptionsFrom(const json& params) {
    const std::string style = params.value("style", "prose");
    std::string detail = params.value("detail", "concise");
    if (detail == "standard") detail = "concise";
    if (style != "prose" && style != "soap") return InvalidParams("unknown style: " + style);
    if (detail != "concise" && detail != "detailed") {
        return InvalidParams("unknown detail: " + detail);
    }
    clinicavt::note::NoteOptions options{style, detail};
    options.confirmed = params.value("confirmed", false);
    return options;
}

Notify QueueTo(PipeServer& server) {
    return [&server](const std::string& method, json params) {
        server.QueueNotification(method, std::move(params));
    };
}

// The note and patient lanes announce themselves when a writer is wired. Without one the stubs
// keep the contract for CI
void StubDocuments(const Notify& notify) {
    notify("note/ready", json::object());
    notify("patient/ready", json::object());
}

std::optional<std::filesystem::path> RecordingPath(const json& params) {
    if (!params.contains("path") || !params["path"].is_string() ||
        params["path"].get_ref<const std::string&>().empty()) {
        return std::nullopt;
    }
    return clinicavt::utf8::ToPath(params["path"].get_ref<const std::string&>());
}

// The reader's reason is plain words. Any other failure may name the file
template <class Read>
auto ReadRecording(Read read) -> std::variant<decltype(read()), Error> {
    try {
        return read();
    } catch (const clinicavt::audio::RecordingError& e) {
        return SessionError(e.what());
    } catch (const std::exception&) {
        return SessionError("the recording could not be read");
    }
}

}  // namespace

json ImportProgressJson(const std::string& id, clinicavt::session::ImportStage stage, int percent) {
    using clinicavt::session::ImportStage;
    const char* name = stage == ImportStage::kReading        ? "reading"
                       : stage == ImportStage::kSpeech       ? "speech"
                       : stage == ImportStage::kTranscribing ? "transcribing"
                                                             : "finalising";
    return json{{"sessionId", id}, {"stage", name}, {"percent", percent}};
}

std::variant<json, Error> HandleRecordingInspect(clinicavt::audio::IRecordingReader& reader,
                                                 const json& params) {
    const auto path = RecordingPath(params);
    if (!path) return InvalidParams("path must be a file path");
    const auto info = ReadRecording([&] { return reader.Inspect(*path); });
    if (std::holds_alternative<Error>(info)) return std::get<Error>(info);
    const auto& recording = std::get<clinicavt::audio::RecordingInfo>(info);
    return json{{"seconds", recording.seconds},
                {"recordedAt", clinicavt::Iso8601(recording.recorded_at)}};
}

std::variant<json, Error> HandleSessionImport(clinicavt::audio::IRecordingReader& reader,
                                              clinicavt::session::SessionController& controller,
                                              clinicavt::translate::ITranslator* translator,
                                              const Notify& push, const json& params) {
    const auto path = RecordingPath(params);
    if (!path) return InvalidParams("path must be a file path");
    // Stores order times as text and restore accepts only this form
    if (!params.contains("startedAt") || !params["startedAt"].is_string() ||
        !clinicavt::archive::IsIso8601(params["startedAt"].get<std::string>())) {
        return InvalidParams("startedAt must be UTC to the second, as 2026-09-26T13:05:00Z");
    }
    const std::string started_at = params["startedAt"].get<std::string>();
    if (params.contains("retain") && !params["retain"].is_boolean()) {
        return InvalidParams("retain must be true or false");
    }
    if (started_at > clinicavt::Iso8601Now()) {
        return SessionError("the recording's date and time are in the future");
    }
    if (controller.Running()) return SessionError("a session is running");
    // A file that cannot be opened is refused here. Its decode runs on the import's thread
    const auto readable = ReadRecording([&] { return reader.Inspect(*path); });
    if (std::holds_alternative<Error>(readable)) return std::get<Error>(readable);
    // The whole recording and its finalise need the memory
    if (translator != nullptr) translator->Release();
    const auto read = [&reader, file = *path](const clinicavt::audio::ReadProgress& progress) {
        try {
            return reader.Decode(file, progress);
        } catch (const clinicavt::audio::RecordingError&) {
            throw;
        } catch (const std::exception&) {
            throw std::runtime_error("the recording could not be read");
        }
    };
    const bool stubs = !controller.HasNoteWriter();
    clinicavt::session::ImportReport report{
        .progress =
            [push, last = std::make_shared<std::pair<int, int>>(-1, -1)](
                const std::string& id, clinicavt::session::ImportStage stage, int percent) {
                // At most one push per percent or stage, so a long file never floods the pipe
                const int now = static_cast<int>(stage);
                if (now == last->first && percent <= last->second) return;
                *last = {now, percent};
                push("session/importProgress", ImportProgressJson(id, stage, percent));
            },
        .done =
            [push, stubs](const std::string& id, const std::string& error) {
                if (!error.empty()) {
                    push("session/importFailed", json{{"sessionId", id}, {"error", error}});
                    return;
                }
                push("session/imported", json{{"sessionId", id}});
                if (stubs) StubDocuments(push);
            }};
    const auto id =
        controller.Import(read, started_at, params.value("retain", true), std::move(report));
    if (id.empty()) {
        return SessionError(
            "a session or a voice enrolment is running, or the session could not be stored");
    }
    return json{{"sessionId", id}};
}

void RegisterSessionMethods(PipeServer& server, const EngineServices& services) {
    auto& controller = services.controller;
    auto& sessions = services.sessions;
    auto* const translator = services.translator;
    auto* const translate_lane = services.translate_lane;
    const auto demo_dir = services.demo_dir;
    auto* const archive_lane = services.archive_lane;
    const auto archive_busy = [archive_lane] {
        return archive_lane != nullptr && archive_lane->Busy();
    };
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
    server.RegisterMethod(
        "session/delete",
        [&sessions, &controller, archive_busy](const json& params) -> std::variant<json, Error> {
            if (archive_busy()) return SessionError(kArchiveRunning);
            if (controller.Busy()) return SessionError("finish the consultation first");
            return HandleSessionDelete(sessions, params);
        });
    // The shell confirms first
    server.RegisterMethod(
        "session/deleteAll", [&sessions, &controller, archive_busy](const json& params) {
            return HandleSessionDeleteAll(sessions, params, controller.Busy(), archive_busy());
        });
    server.RegisterMethod(
        "session/remove",
        [&sessions, &controller, archive_busy](const json& params) -> std::variant<json, Error> {
            if (controller.Busy()) return SessionError("finish the consultation first");
            return HandleSessionRemove(sessions, params, archive_busy());
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
    server.RegisterMethod(
        "demo/clear",
        [&sessions, &controller, archive_busy](const json&) -> std::variant<json, Error> {
            if (archive_busy()) return SessionError(kArchiveRunning);
            if (controller.Busy()) return SessionError("finish the consultation first");
            return HandleDemoClear(sessions);
        });
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
        [&controller, translator](const json& params) -> std::variant<json, Error> {
            // An optional replay block plays a file through the same
            // pipeline. Absent means microphone
            std::optional<clinicavt::session::ReplaySpec> replay;
            if (params.contains("replay")) {
                const auto& r = params["replay"];
                if (!r.contains("path") || !r["path"].is_string()) {
                    return Error{kInvalidParams, "replay.path is required", {}};
                }
                replay = clinicavt::session::ReplaySpec{r["path"].get<std::string>(),
                                                        r.value("speed", 1.0)};
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
    server.RegisterMethod("session/stop",
                          [&server, &controller](const json&) -> std::variant<json, Error> {
                              // Stop would wait out the import, holding every request behind it
                              if (controller.Importing())
                                  return SessionError("an import is running");
                              controller.Stop();
                              if (!controller.HasNoteWriter()) StubDocuments(QueueTo(server));
                              return json{{"sessionId", controller.LastFinalised()}};
                          });
    if (services.recordings != nullptr) {
        auto& reader = *services.recordings;
        server.RegisterMethod("recording/inspect", [&reader](const json& params) {
            return HandleRecordingInspect(reader, params);
        });
        server.RegisterMethod(
            "session/import", [&server, &controller, &reader, translator](const json& params) {
                return HandleSessionImport(reader, controller, translator, PushTo(server), params);
            });
    }
}

}  // namespace clinicavt::ipc
