#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <set>

#include "adapters/guidance/guidance_record.hpp"
#include "adapters/ipc/handlers.hpp"
#include "core/common/log.hpp"
#include "core/common/utf8.hpp"
#include "ports/store_error.hpp"

namespace clinicavt::ipc {
namespace {

json GuidanceReadyJson(const std::string& session, const clinicavt::guidance::Record& record) {
    json body = clinicavt::guidance::ToJson(record);
    body["id"] = NullWhenEmpty(session);
    body["storeError"] = nullptr;
    body["stale"] = nullptr;
    return body;
}

const char* PhaseName(clinicavt::guidance::Readiness::Phase phase) {
    switch (phase) {
        case clinicavt::guidance::Readiness::Phase::kLoading:
            return "loading";
        case clinicavt::guidance::Readiness::Phase::kReady:
            return "ready";
        case clinicavt::guidance::Readiness::Phase::kUnavailable:
            return "unavailable";
    }
    return "unavailable";
}

}  // namespace

json GuidanceModelJson(const clinicavt::guidance::Readiness& readiness) {
    return json{{"state", PhaseName(readiness.phase)}, {"detail", NullWhenEmpty(readiness.detail)}};
}

json GuidanceCorporaJson(const clinicavt::guidance::Readiness& readiness,
                         const std::vector<clinicavt::guidance::Corpus>& corpora) {
    json result = GuidanceModelJson(readiness);
    json list = json::array();
    for (const auto& c : corpora) list.push_back(clinicavt::guidance::ToJson(c));
    result["corpora"] = list;
    return result;
}

clinicavt::guidance::SearchRequest GuidanceSearchRequest(
    clinicavt::store::IDocumentStore& documents, const std::string& session,
    clinicavt::store::Document note, int limit, const Notify& notify) {
    clinicavt::guidance::SearchRequest request;
    request.session = session;
    request.note = std::move(note.text);
    request.limit = limit;
    const auto revision = note.revision;
    request.on_ready = [&documents, session, revision,
                        notify](const clinicavt::guidance::Results& results) {
        using clinicavt::store::DocumentKind;
        const clinicavt::guidance::Record record{results, revision};
        json body = GuidanceReadyJson(session, record);
        if (!session.empty()) {
            try {
                documents.SaveDocument(session, DocumentKind::kGuidance,
                                       {.text = clinicavt::guidance::Dump(record)});
            } catch (const clinicavt::store::StoreError& e) {
                if (e.Code() == clinicavt::store::StoreCode::kNotFound) {
                    log::Printf("clinicavt-engine: guidance for %s dropped, session gone\n",
                                session.c_str());
                    return;
                }
                body["storeError"] = e.what();
            } catch (const std::exception& e) {
                body["storeError"] = e.what();
            }
            // Leave stale unset if the note cannot be read
            try {
                body["stale"] =
                    documents.ReadDocument(session, DocumentKind::kNote).revision != revision;
            } catch (const clinicavt::store::StoreError& e) {
                if (e.Code() == clinicavt::store::StoreCode::kNotFound) return;
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch) stale stays unknown
            }
        }
        notify("guidance/ready", std::move(body));
    };
    request.on_failed = [session, notify](const std::string& detail) {
        notify("guidance/failed", json{{"id", NullWhenEmpty(session)}, {"detail", detail}});
    };
    return request;
}

std::variant<json, Error> HandleGuidanceSearch(clinicavt::store::IDocumentStore& documents,
                                               clinicavt::guidance::IGuidanceLane& lane,
                                               const json& params, const Notify& notify) {
    int limit = kGuidanceLimit;
    if (params.contains("limit")) {
        if (!params["limit"].is_number_integer() || params["limit"].get<int>() < 1 ||
            params["limit"].get<int>() > 20) {
            return InvalidParams("limit must be 1-20");
        }
        limit = params["limit"].get<int>();
    }
    std::string session;
    bool as_note = false;
    clinicavt::store::Document note;
    if (params.contains("text")) {
        if (!params["text"].is_string()) {
            return InvalidParams("text must be a string");
        }
        note.text = params["text"].get<std::string>();
        // Typed text is embedded whole. mode "note" splits it into sub-queries like a stored note
        if (params.contains("mode")) {
            if (params["mode"] != "query" && params["mode"] != "note") {
                return InvalidParams("mode must be query or note");
            }
            as_note = params["mode"] == "note";
        }
    } else {
        const auto id = IdFrom(params);
        if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
        session = std::get<std::string>(id);
        try {
            note = documents.ReadDocument(session, clinicavt::store::DocumentKind::kNote);
        } catch (const std::exception& e) {
            return SessionError(e.what());
        }
    }
    if (note.text.empty()) return SessionError("no note to search");
    auto request = GuidanceSearchRequest(documents, session, std::move(note), limit, notify);
    request.as_note = as_note;
    lane.Run(std::move(request));
    return json::object();
}

namespace {

// Compares only added documents ("upload:" ids), ignoring corpora
bool DocumentsChangedSince(const clinicavt::guidance::Record& record,
                           clinicavt::guidance::IDocumentIngest& ingest) {
    std::set<std::string> searched;
    for (const auto& c : record.results.searched) {
        if (c.id.starts_with("upload:")) searched.insert(c.id);
    }
    std::set<std::string> ready;
    for (const auto& d : ingest.List().documents) {
        if (d.state == clinicavt::guidance::DocumentState::kReady)
            ready.insert("upload:" + std::to_string(d.id));
    }
    return searched != ready;
}

}  // namespace

std::variant<json, Error> HandleSessionGuidance(clinicavt::store::IDocumentStore& documents,
                                                const json& params,
                                                clinicavt::guidance::IDocumentIngest* ingest) {
    return WithSession(params, [&](const std::string& session) {
        using clinicavt::store::DocumentKind;
        const auto stored = documents.ReadDocument(session, DocumentKind::kGuidance);
        if (stored.text.empty()) return json{{"guidance", nullptr}};
        const json parsed = json::parse(stored.text, nullptr, false);
        std::optional<clinicavt::guidance::Record> record;
        if (clinicavt::guidance::CanRead(parsed)) {
            try {
                record = clinicavt::guidance::RecordFromJson(parsed);
            } catch (const json::exception&) {  // NOLINT(bugprone-empty-catch) read as none
            }
        }
        if (!record) {
            log::Printf("clinicavt-engine: guidance record for %s unreadable, dropped\n",
                        session.c_str());
            return json{{"guidance", nullptr}};
        }
        json guidance = clinicavt::guidance::ToJson(*record);
        guidance["stale"] =
            record->note_revision != documents.ReadDocument(session, DocumentKind::kNote).revision;
        guidance["documentsChanged"] = ingest != nullptr && DocumentsChangedSince(*record, *ingest);
        return json{{"guidance", guidance}};
    });
}

json DocumentJson(const clinicavt::guidance::DocumentInfo& document) {
    return json{{"id", document.id},
                {"name", document.name},
                {"path", document.path},
                {"state", clinicavt::guidance::DocumentStateName(document.state)},
                {"error", NullWhenEmpty(document.error)},
                {"addedAt", document.added_at},
                {"pages", document.pages},
                {"pagesWithoutText", document.pages_without_text},
                {"chunks", document.chunks}};
}

json ProgressJson(const clinicavt::guidance::IngestProgress& progress) {
    return json{{"id", progress.id},
                {"phase", progress.phase},
                {"done", progress.done},
                {"total", progress.total}};
}

bool ChangesReadySet(const clinicavt::guidance::DocumentInfo& document) {
    using clinicavt::guidance::DocumentState;
    return document.state == DocumentState::kReady ||
           (document.state == DocumentState::kRemoved && document.chunks > 0);
}

namespace {

Error DocumentsError(const std::exception& e) {
    return Error{kSessionError, "Document store error", json(e.what())};
}

// kNotFound -> invalid params; any other store error -> documents error
std::variant<json, Error> DocumentsRefused(const std::exception& e) {
    if (const auto* store = dynamic_cast<const clinicavt::store::StoreError*>(&e);
        store != nullptr && store->Code() == clinicavt::store::StoreCode::kNotFound) {
        return InvalidParams("unknown document");
    }
    return DocumentsError(e);
}

json Param(const json& params, const char* key) {
    return params.is_object() ? params.value(key, json()) : json();
}

}  // namespace

std::variant<json, Error> HandleDocumentsAdd(clinicavt::guidance::IDocumentIngest& ingest,
                                             const json& params) {
    const json paths = Param(params, "paths");
    auto invalid = InvalidParams("paths must be a list of strings");
    if (!paths.is_array() || paths.empty()) return invalid;
    std::vector<std::filesystem::path> files;
    for (const auto& path : paths) {
        if (!path.is_string()) return invalid;
        const auto text = path.get<std::string>();
        files.push_back(clinicavt::utf8::ToPath(text));
    }
    try {
        const auto accepted = ingest.Add(files);
        json documents = json::array();
        for (const auto& document : accepted.documents) documents.push_back(DocumentJson(document));
        json skipped = json::array();
        for (const auto& s : accepted.skipped) {
            skipped.push_back(json{{"path", s.path}, {"reason", s.reason}});
        }
        return json{{"documents", documents}, {"skipped", skipped}};
    } catch (const std::exception& e) {
        return DocumentsError(e);
    }
}

std::variant<json, Error> HandleDocumentsList(clinicavt::guidance::IDocumentIngest& ingest) {
    try {
        const auto listing = ingest.List();
        json documents = json::array();
        for (const auto& document : listing.documents) documents.push_back(DocumentJson(document));
        return json{{"folder", clinicavt::utf8::FromPath(listing.folder)},
                    {"found", listing.found},
                    {"unsupported", listing.unsupported},
                    {"documents", documents}};
    } catch (const std::exception& e) {
        return DocumentsError(e);
    }
}

std::variant<json, Error> HandleDocumentsRemove(clinicavt::guidance::IDocumentIngest& ingest,
                                                const json& params) {
    const json id = Param(params, "id");
    if (!id.is_number_integer()) {
        return InvalidParams("id must be an integer");
    }
    try {
        ingest.Remove(id.get<std::int64_t>());
        return json::object();
    } catch (const std::exception& e) {
        return DocumentsRefused(e);
    }
}

std::variant<json, Error> HandleDocumentsPage(clinicavt::guidance::IDocumentIngest& ingest,
                                              const json& params) {
    const json id = Param(params, "id");
    const json page = Param(params, "page");
    const std::string chunk_id = params.is_object() ? params.value("chunkId", "") : "";
    // The chunk id is "upload:<document>-<ord>"
    const auto dash = chunk_id.rfind('-');
    const bool numbered =
        dash != std::string::npos && dash + 1 < chunk_id.size() &&
        std::all_of(chunk_id.begin() + static_cast<std::ptrdiff_t>(dash) + 1, chunk_id.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; });
    if (!id.is_number_integer() || !page.is_number_integer() || page.get<int>() < 0 || !numbered) {
        return InvalidParams("id, page and chunkId are required");
    }
    try {
        const auto ord = std::stoll(chunk_id.substr(dash + 1));
        const auto drawn = ingest.Render(id.get<std::int64_t>(), page.get<int>(), ord);
        return json{{"path", clinicavt::utf8::FromPath(drawn.path)},
                    {"width", drawn.width},
                    {"height", drawn.height},
                    {"pages", drawn.pages},
                    {"boxes", json::parse(drawn.boxes)}};
    } catch (const std::exception& e) {
        return DocumentsRefused(e);
    }
}

std::variant<json, Error> HandleDocumentsOpen(clinicavt::guidance::IDocumentIngest& ingest,
                                              const json& params) {
    const json id = Param(params, "id");
    if (!id.is_number_integer()) {
        return InvalidParams("id must be an integer");
    }
    try {
        return json{{"path", clinicavt::utf8::FromPath(ingest.Path(id.get<std::int64_t>()))}};
    } catch (const std::exception& e) {
        return DocumentsRefused(e);
    }
}

void RegisterGuidanceMethods(PipeServer& server, clinicavt::store::IDocumentStore& documents,
                             clinicavt::guidance::IGuidanceRetriever& retriever,
                             clinicavt::guidance::IGuidanceLane& lane,
                             clinicavt::guidance::IDocumentIngest& ingest) {
    server.RegisterMethod("guidance/search", [&server, &documents, &lane](const json& params) {
        return HandleGuidanceSearch(documents, lane, params, PushTo(server));
    });
    server.RegisterMethod("guidance/corpora", [&retriever](const json&) {
        return GuidanceCorporaJson(retriever.Status(), retriever.Corpora());
    });
    // Dev only: reloads corpora with or without research ones, no restart
    server.RegisterMethod("guidance/research",
                          [&server, &retriever](const json& params) -> std::variant<json, Error> {
                              const json include = Param(params, "include");
                              if (!include.is_boolean()) {
                                  return InvalidParams("include must be a boolean");
                              }
                              retriever.SetResearch(include.get<bool>());
                              server.PushNotification("guidance/model",
                                                      GuidanceModelJson(retriever.Status()));
                              return json::object();
                          });
    server.RegisterMethod("session/guidance", [&documents, &ingest](const json& params) {
        return HandleSessionGuidance(documents, params, &ingest);
    });
    server.RegisterMethod("guidance/documents/add", [&ingest](const json& params) {
        return HandleDocumentsAdd(ingest, params);
    });
    server.RegisterMethod("guidance/documents",
                          [&ingest](const json&) { return HandleDocumentsList(ingest); });
    server.RegisterMethod("guidance/documents/remove", [&ingest](const json& params) {
        return HandleDocumentsRemove(ingest, params);
    });
    server.RegisterMethod("guidance/page", [&ingest](const json& params) {
        return HandleDocumentsPage(ingest, params);
    });
    server.RegisterMethod("guidance/documents/open", [&ingest](const json& params) {
        return HandleDocumentsOpen(ingest, params);
    });
    server.RegisterMethod("guidance/documents/removeAll",
                          [&ingest](const json&) -> std::variant<json, Error> {
                              try {
                                  return json{{"removed", ingest.RemoveAll()}};
                              } catch (const std::exception& e) {
                                  return DocumentsError(e);
                              }
                          });
}

}  // namespace clinicavt::ipc
