#include "adapters/guidance/guidance_record.hpp"

#include <cmath>

namespace clinicavt::guidance {
namespace {

// Sends an empty string as null
json OrNull(const std::string& s) {
    if (s.empty()) return nullptr;
    return s;
}

std::string Str(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    return it->get<std::string>();
}

int Int(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return 0;
    return it->get<int>();
}

double Num(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return 0;
    return it->get<double>();
}

// Document ids are 63-bit and do not fit a double exactly
std::int64_t Int64(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return 0;
    return it->get<std::int64_t>();
}

}  // namespace

json ToJson(const Corpus& c) {
    return json{{"id", c.id},
                {"name", c.name},
                {"licence", c.licence},
                {"attribution", c.attribution},
                {"label", c.label},
                {"source", c.source},
                {"research", c.research},
                {"embedder", c.embedder},
                {"sha256", c.sha256},
                {"chunks", c.chunks},
                {"builtAt", OrNull(c.built_at)},
                {"unavailable", OrNull(c.unavailable)}};
}

json ToJson(const Results& results) {
    json shown = json::array();
    for (const auto& r : results.shown) {
        shown.push_back({{"corpus", r.corpus},
                         {"chunkId", r.chunk_id},
                         {"code", r.code},
                         {"number", r.number},
                         {"title", r.title},
                         {"section", r.section},
                         {"text", r.text},
                         {"url", r.url},
                         {"lastUpdated", r.last_updated},
                         {"updateTag", r.update_tag},
                         {"source", r.source},
                         {"citation", r.citation},
                         {"score", std::round(r.score * 1000) / 1000},
                         {"trigger", r.trigger},
                         {"document", r.document},
                         {"page", r.page},
                         {"pages", r.pages}});
    }
    json searched = json::array();
    for (const auto& c : results.searched) searched.push_back(ToJson(c));
    return json{{"version", kRecordVersion},
                {"shown", shown},
                {"searched", searched},
                {"considered", results.considered},
                {"floor", results.floor},
                {"abstained", results.abstained},
                {"uploadFloor", results.upload_floor}};
}

namespace {

Corpus CorpusFromJson(const json& j) {
    Corpus c;
    c.id = Str(j, "id");
    c.name = Str(j, "name");
    c.licence = Str(j, "licence");
    c.attribution = Str(j, "attribution");
    c.label = Str(j, "label");
    c.source = Str(j, "source");
    c.research = j.value("research", false);
    c.embedder = Str(j, "embedder");
    c.sha256 = Str(j, "sha256");
    c.chunks = Int(j, "chunks");
    c.built_at = Str(j, "builtAt");
    c.unavailable = Str(j, "unavailable");
    return c;
}

}  // namespace

Results FromJson(const json& j) {
    Results out;
    if (const auto shown = j.find("shown"); shown != j.end() && shown->is_array()) {
        for (const auto& r : *shown) {
            Result result;
            result.corpus = Str(r, "corpus");
            result.chunk_id = Str(r, "chunkId");
            result.code = Str(r, "code");
            result.number = Str(r, "number");
            result.title = Str(r, "title");
            result.section = Str(r, "section");
            result.text = Str(r, "text");
            result.url = Str(r, "url");
            result.last_updated = Str(r, "lastUpdated");
            result.update_tag = Str(r, "updateTag");
            result.source = Str(r, "source");
            result.citation = Str(r, "citation");
            result.score = Num(r, "score");
            result.trigger = Str(r, "trigger");
            result.document = Int64(r, "document");
            result.page = Int(r, "page");
            result.pages = Int(r, "pages");
            out.shown.push_back(std::move(result));
        }
    }
    if (const auto searched = j.find("searched"); searched != j.end() && searched->is_array()) {
        for (const auto& c : *searched) out.searched.push_back(CorpusFromJson(c));
    }
    out.considered = Int(j, "considered");
    out.floor = Num(j, "floor");
    out.upload_floor = Num(j, "uploadFloor");
    if (const auto it = j.find("abstained"); it != j.end() && it->is_boolean()) {
        out.abstained = it->get<bool>();
    }
    return out;
}

json ToJson(const Record& record) {
    json body = ToJson(record.results);
    body["noteRevision"] = record.note_revision;
    return body;
}

Record RecordFromJson(const json& j) {
    Record record;
    record.results = FromJson(j);
    if (const auto it = j.find("noteRevision"); it != j.end() && it->is_number_integer()) {
        record.note_revision = it->get<std::int64_t>();
    }
    return record;
}

std::string Dump(const Record& record) {
    return ToJson(record).dump(-1, ' ', false, json::error_handler_t::replace);
}

bool CanRead(const json& j) {
    if (!j.is_object()) return false;
    const auto version = j.find("version");
    if (version != j.end()) {
        if (!version->is_number_integer()) return false;
        const auto v = version->get<std::int64_t>();
        if (v < 1 || v > kRecordVersion) return false;
    }
    const auto shown = j.find("shown");
    const auto abstained = j.find("abstained");
    return shown != j.end() && shown->is_array() && abstained != j.end() && abstained->is_boolean();
}

}  // namespace clinicavt::guidance
