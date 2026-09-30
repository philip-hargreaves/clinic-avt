#include <string>

#include "adapters/interfaces/translator.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/translate/translate_lane.hpp"

namespace clinicavt::ipc {

void RegisterTranslateMethods(PipeServer& server, const EngineServices& services) {
    auto& documents = services.documents;
    auto* const translator = services.translator;
    auto* const translate_lane = services.translate_lane;
    if (translator == nullptr || translate_lane == nullptr) return;
    server.RegisterMethod("translate/languages", [translator](const json&) {
        return json{{"languages", translator->Languages()}};
    });
    // Runs off the RPC thread and sends results as translate/partial, then translate/ready
    server.RegisterMethod("patient/translate", [&documents, translate_lane](const json& params) {
        return WithSession(params, [&](const std::string& session_id) -> std::variant<json, Error> {
            if (!params.contains("language") || !params["language"].is_string()) {
                return InvalidParams("language must be a string");
            }
            const auto text =
                documents.ReadDocument(session_id, clinicavt::store::DocumentKind::kPatient).text;
            if (text.empty()) {
                return SessionError("no patient information to translate");
            }
            // Store before translate/ready so a read after it sees the translation
            const auto on_ready = [&documents, session_id](const std::string& translated,
                                                           const std::string& language) {
                try {
                    clinicavt::store::Document document;
                    document.text = translated;
                    document.language = language;
                    documents.SaveDocument(session_id, clinicavt::store::DocumentKind::kTranslation,
                                           document);
                } catch (...) {  // NOLINT(bugprone-empty-catch) storing is best effort
                }
            };
            if (!translate_lane->Run(text, params["language"].get<std::string>(), on_ready)) {
                return SessionError("a translation is already running");
            }
            return json::object();
        });
    });
}

}  // namespace clinicavt::ipc
