#include <optional>
#include <string>

#include "adapters/archive/archive_lane.hpp"
#include "adapters/archive/archive_password.hpp"
#include "adapters/ipc/handlers.hpp"
#include "core/archive/record_rules.hpp"
#include "core/common/utf8.hpp"

namespace clinicavt::ipc {

namespace {

// An instant bounding a period. The shell sends "" for an open end, or leaves it out
bool TimeParam(const json& params, const char* key, std::string& out) {
    if (!params.contains(key) || params[key].is_null()) {
        out.clear();
        return true;
    }
    if (!params[key].is_string()) return false;
    out = params[key].get<std::string>();
    return out.empty() || clinicavt::archive::IsIso8601(out);
}

std::variant<clinicavt::archive::Period, Error> PeriodFrom(const json& params) {
    clinicavt::archive::Period period;
    if (params.is_null()) return period;
    if (!params.is_object() || !TimeParam(params, "from", period.from) ||
        !TimeParam(params, "to", period.to)) {
        return InvalidParams("from and to must be ISO 8601 UTC instants or empty");
    }
    return period;
}

// What refuses a backup or restore before it starts
std::optional<Error> Refusal(bool session_active, const clinicavt::archive::ArchiveLane& lane) {
    if (session_active) return SessionError("finish the consultation first");
    if (lane.Busy()) return SessionError("a backup or restore is already running");
    return std::nullopt;
}

bool StringParam(const json& params, const char* key) {
    return params.contains(key) && params[key].is_string();
}

}  // namespace

std::variant<json, Error> HandleArchiveSummary(clinicavt::store::ISessionStore& sessions,
                                               const json& params) {
    const auto period = PeriodFrom(params);
    if (std::holds_alternative<Error>(period)) return std::get<Error>(period);
    // The last backup, when there is one: what it held and when it was made
    std::optional<clinicavt::archive::Period> covered;
    std::string at;
    if (params.contains("covered") && !params["covered"].is_null()) {
        const auto given = PeriodFrom(params["covered"]);
        if (std::holds_alternative<Error>(given) || !StringParam(params["covered"], "at") ||
            !clinicavt::archive::IsIso8601(params["covered"]["at"].get<std::string>())) {
            return InvalidParams("covered needs from, to and an ISO 8601 UTC at");
        }
        covered = std::get<clinicavt::archive::Period>(given);
        at = params["covered"]["at"].get<std::string>();
    }
    try {
        const auto counts =
            clinicavt::archive::Summarise(sessions, std::get<clinicavt::archive::Period>(period));
        const std::size_t uncovered =
            covered ? clinicavt::archive::Uncovered(sessions, *covered, at) : 0;
        return json{{"consultations", counts.consultations},
                    {"reflections", counts.reflections},
                    {"unfinished", counts.unfinished},
                    {"uncovered", uncovered}};
    } catch (const std::exception& e) {
        return SessionError(e.what());
    }
}

std::variant<json, Error> HandleArchiveBackup(clinicavt::archive::ArchiveLane& lane,
                                              bool session_active, const json& params) {
    const auto period = PeriodFrom(params);
    if (std::holds_alternative<Error>(period)) return std::get<Error>(period);
    if (!StringParam(params, "path") || params["path"].get_ref<const std::string&>().empty()) {
        return InvalidParams("path must be a file path");
    }
    if (!StringParam(params, "password")) return InvalidParams("password must be a string");
    if (params.contains("reflectionsOnly") && !params["reflectionsOnly"].is_boolean()) {
        return InvalidParams("reflectionsOnly must be true or false");
    }
    if (const auto refused = Refusal(session_active, lane)) return *refused;
    std::string password = params["password"].get<std::string>();
    clinicavt::archive::WipeOnExit wipe{password};
    if (!lane.BackUp(std::get<clinicavt::archive::Period>(period),
                     clinicavt::utf8::ToPath(params["path"].get<std::string>()), password,
                     params.value("reflectionsOnly", false))) {
        return SessionError("a backup or restore is already running");
    }
    return json::object();
}

std::variant<json, Error> HandleArchiveRestore(clinicavt::archive::ArchiveLane& lane,
                                               bool session_active, const json& params) {
    if (!params.is_object() || !StringParam(params, "path") ||
        params["path"].get_ref<const std::string&>().empty()) {
        return InvalidParams("path must be a file path");
    }
    if (!StringParam(params, "password")) return InvalidParams("password must be a string");
    if (params.contains("dryRun") && !params["dryRun"].is_boolean()) {
        return InvalidParams("dryRun must be true or false");
    }
    if (const auto refused = Refusal(session_active, lane)) return *refused;
    std::string password = params["password"].get<std::string>();
    clinicavt::archive::WipeOnExit wipe{password};
    if (!lane.Restore(clinicavt::utf8::ToPath(params["path"].get<std::string>()), password,
                      params.value("dryRun", false))) {
        return SessionError("a backup or restore is already running");
    }
    return json::object();
}

void RegisterArchiveMethods(PipeServer& server, const EngineServices& services) {
    auto& controller = services.controller;
    auto& sessions = services.sessions;
    auto* const lane = services.archive_lane;
    server.RegisterMethod("archive/summary", [&sessions](const json& params) {
        return HandleArchiveSummary(sessions, params);
    });
    // Both reply at once; the job reports on archive/progress, then archive/done or failed
    server.RegisterMethod("archive/backup", [&controller, lane](const json& params) {
        return HandleArchiveBackup(*lane, controller.Running(), params);
    });
    server.RegisterMethod("archive/restore", [&controller, lane](const json& params) {
        return HandleArchiveRestore(*lane, controller.Running(), params);
    });
}

}  // namespace clinicavt::ipc
