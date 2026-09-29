#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "adapters/interfaces/translator.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/ipc/session_common.hpp"
#include "core/archive/record_rules.hpp"
#include "core/common/iso8601.hpp"
#include "core/common/utf8.hpp"

namespace clinicavt::ipc {

namespace {

std::optional<std::filesystem::path> RecordingPath(const json& params) {
    if (!params.contains("path") || !params["path"].is_string() ||
        params["path"].get_ref<const std::string&>().empty()) {
        return std::nullopt;
    }
    return clinicavt::utf8::ToPath(params["path"].get_ref<const std::string&>());
}

// RecordingError text is plain; other errors may name the file
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
                                              const Notify& push, const json& params,
                                              const std::vector<std::string>& missing) {
    const auto path = RecordingPath(params);
    if (!path) return InvalidParams("path must be a file path");
    // Stored times sort as text, and restore accepts only this form
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
    if (!missing.empty()) return SessionError(MissingModelsReason(missing));
    if (controller.Running()) return SessionError("a session is running");
    // Reject unopenable files here; decoding runs on the import thread
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
                // One push per percent or stage change at most, so the pipe isn't flooded
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

void RegisterImportMethods(PipeServer& server, const EngineServices& services) {
    if (services.recordings == nullptr) return;
    auto& controller = services.controller;
    auto* const translator = services.translator;
    auto& reader = *services.recordings;
    server.RegisterMethod("recording/inspect", [&reader](const json& params) {
        return HandleRecordingInspect(reader, params);
    });
    server.RegisterMethod("session/import", [&server, &controller, &reader, translator,
                                             missing =
                                                 services.missing_models](const json& params) {
        return HandleSessionImport(reader, controller, translator, PushTo(server), params, missing);
    });
}

}  // namespace clinicavt::ipc
