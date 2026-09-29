#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "adapters/ipc/handlers.hpp"
#include "adapters/ipc/session_common.hpp"

namespace clinicavt::ipc {

namespace {

constexpr const char* kAnswers[] = {"happened", "learned", "next"};

std::string StringField(const json& given, const char* field) {
    return given.is_object() && given.contains(field) && given[field].is_string()
               ? given[field].get<std::string>()
               : std::string();
}

clinicavt::records::Reference ReferenceFrom(const json& given) {
    return {StringField(given, "key"), StringField(given, "reference"), StringField(given, "title"),
            StringField(given, "link"), StringField(given, "source")};
}

json ReferenceJson(const clinicavt::records::Reference& reference) {
    return {{"key", reference.key},
            {"reference", reference.reference},
            {"title", reference.title},
            {"link", reference.link},
            {"source", reference.source}};
}

json AnswersJson(const clinicavt::records::Answers& answers) {
    json result{
        {"happened", answers.happened}, {"learned", answers.learned}, {"next", answers.next}};
    result["references"] = json::array();
    if (answers.references) {
        for (const auto& reference : *answers.references) {
            result["references"].push_back(ReferenceJson(reference));
        }
    }
    return result;
}

}  // namespace

std::variant<json, Error> HandleReflectionGet(clinicavt::records::Reflections& reflections,
                                              const json& params) {
    return WithSession(params, [&](const std::string& session) {
        const auto reflection = reflections.Get(session);
        json result{{"id", session},
                    {"label", reflection.label},
                    {"summary", nullptr},
                    {"reflection", nullptr}};
        if (reflection.summary) {
            result["summary"] = {{"text", reflection.summary->text},
                                 {"generatedAt", NullWhenEmpty(reflection.summary->generated_at)},
                                 {"editedAt", NullWhenEmpty(reflection.summary->edited_at)}};
        }
        if (reflection.entry) {
            json entry = AnswersJson(reflection.entry->answers);
            entry["createdAt"] = NullWhenEmpty(reflection.entry->created_at);
            entry["editedAt"] = NullWhenEmpty(reflection.entry->edited_at);
            result["reflection"] = std::move(entry);
        }
        return result;
    });
}

std::variant<json, Error> HandleReflectionUpdate(clinicavt::records::Reflections& reflections,
                                                 const json& params) {
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
        clinicavt::records::ReflectionEdit edit;
        if (params.contains("happened")) edit.happened = params["happened"].get<std::string>();
        if (params.contains("learned")) edit.learned = params["learned"].get<std::string>();
        if (params.contains("next")) edit.next = params["next"].get<std::string>();
        if (params.contains("summary")) edit.summary = params["summary"].get<std::string>();
        if (params.contains("references")) {
            edit.references.emplace();
            for (const auto& given : params["references"]) {
                edit.references->push_back(ReferenceFrom(given));
            }
        }
        reflections.Update(session, edit);
        return json::object();
    });
}

std::variant<json, Error> HandleReflectionDelete(clinicavt::records::Reflections& reflections,
                                                 const json& params) {
    return WithSession(params, [&](const std::string& id) {
        reflections.Delete(id);
        return json::object();
    });
}

json HandleReflectionList(clinicavt::records::Reflections& reflections) {
    json list = json::array();
    for (const auto& row : reflections.List()) {
        list.push_back({{"id", row.id},
                        {"startedAt", row.started_at},
                        {"label", row.label},
                        {"happened", row.answers.happened},
                        {"learned", row.answers.learned},
                        {"next", row.answers.next},
                        {"summary", row.summary},
                        {"createdAt", NullWhenEmpty(row.created_at)},
                        {"editedAt", NullWhenEmpty(row.edited_at)},
                        {"demo", row.demo}});
    }
    return json{{"reflections", std::move(list)}};
}

std::variant<json, Error> HandleDemoSeed(clinicavt::demo::DemoSamples& demo) {
    try {
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        return json{{"added", demo.SeedOnce(now)}};
    } catch (const std::exception& e) {
        return SessionError(e.what());
    }
}

json HandleDemoClear(clinicavt::demo::DemoSamples& demo) {
    return json{{"removed", demo.Clear()}};
}

void RegisterReflectionMethods(PipeServer& server, const EngineServices& services) {
    auto& controller = services.controller;
    auto& reflections = services.reflections;
    auto& demo = services.demo;
    const auto archive_busy = ArchiveBusy(services);
    server.RegisterMethod("reflection/get", [&reflections](const json& params) {
        return HandleReflectionGet(reflections, params);
    });
    server.RegisterMethod("reflection/update", [&reflections](const json& params) {
        return HandleReflectionUpdate(reflections, params);
    });
    server.RegisterMethod("reflection/delete", [&reflections](const json& params) {
        return HandleReflectionDelete(reflections, params);
    });
    server.RegisterMethod("reflection/list", [&reflections](const json&) {
        return HandleReflectionList(reflections);
    });
    server.RegisterMethod("demo/seed", [&demo](const json&) { return HandleDemoSeed(demo); });
    server.RegisterMethod(
        "demo/clear", [&demo, &controller, archive_busy](const json&) -> std::variant<json, Error> {
            if (archive_busy()) return SessionError(kArchiveRunning);
            if (controller.Busy()) return SessionError("finish the consultation first");
            return HandleDemoClear(demo);
        });
    // Runs on the note lane; result sent as reflection/summary
    server.RegisterMethod(
        "reflection/summary", [&controller](const json& params) -> std::variant<json, Error> {
            const auto id = IdFrom(params);
            if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
            if (!controller.WriteSummary(std::get<std::string>(id))) {
                return SessionError("no stored note, or a document is already being written");
            }
            return json::object();
        });
}

}  // namespace clinicavt::ipc
