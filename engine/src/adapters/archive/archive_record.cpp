#include "adapters/archive/archive_record.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace clinicavt::archive {

namespace {

struct KindName {
    store::DocumentKind kind;
    const char* name;
};

// The store's names, so a backup reads the same as the rows it came from
constexpr KindName kKinds[] = {
    {store::DocumentKind::kNote, "note"},
    {store::DocumentKind::kPatient, "patient"},
    {store::DocumentKind::kTranslation, "translation"},
    {store::DocumentKind::kLabel, "label"},
    {store::DocumentKind::kSummary, "summary"},
    {store::DocumentKind::kReflection, "reflection"},
    {store::DocumentKind::kGuidance, "guidance"},
};

const char* NameOf(store::DocumentKind kind) {
    for (const auto& k : kKinds) {
        if (k.kind == kind) return k.name;
    }
    throw std::invalid_argument("unknown document kind");
}

// No text from the record goes into the error: only the fixed code
[[noreturn]] void Damaged() {
    throw ArchiveError(ArchiveCode::kDamaged);
}

const json& Field(const json& j, const char* key) {
    if (!j.is_object()) Damaged();
    const auto it = j.find(key);
    if (it == j.end()) Damaged();
    return *it;
}

std::string Str(const json& j, const char* key) {
    const json& v = Field(j, key);
    if (!v.is_string()) Damaged();
    return v.get<std::string>();
}

bool Bool(const json& j, const char* key) {
    const json& v = Field(j, key);
    if (!v.is_boolean()) Damaged();
    return v.get<bool>();
}

std::uint64_t Unsigned(const json& j, const char* key) {
    const json& v = Field(j, key);
    if (!v.is_number_unsigned()) Damaged();
    return v.get<std::uint64_t>();
}

std::int64_t Signed(const json& j, const char* key, std::int64_t min, std::int64_t max) {
    const json& v = Field(j, key);
    std::int64_t value = 0;
    if (v.is_number_unsigned()) {
        const auto u = v.get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) Damaged();
        value = static_cast<std::int64_t>(u);
    } else if (v.is_number_integer()) {
        value = v.get<std::int64_t>();
    } else {
        Damaged();
    }
    if (value < min || value > max) Damaged();
    return value;
}

int Int(const json& j, const char* key) {
    return static_cast<int>(
        Signed(j, key, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
}

const json& Array(const json& j, const char* key) {
    const json& v = Field(j, key);
    if (!v.is_array()) Damaged();
    return v;
}

store::DocumentKind KindOf(const std::string& name) {
    for (const auto& k : kKinds) {
        if (name == k.name) return k.kind;
    }
    Damaged();
}

}  // namespace

json ToJson(const Manifest& m) {
    return json{{"version", m.version},
                {"createdAt", m.created_at},
                {"from", m.from},
                {"to", m.to},
                {"consultations", m.consultations},
                {"transcripts", m.transcripts},
                {"appVersion", m.app_version}};
}

Manifest ManifestFromJson(const json& j) {
    try {
        Manifest m;
        m.version = Int(j, "version");
        m.created_at = Str(j, "createdAt");
        m.from = Str(j, "from");
        m.to = Str(j, "to");
        m.consultations = static_cast<std::size_t>(Unsigned(j, "consultations"));
        m.transcripts = Bool(j, "transcripts");
        m.app_version = Str(j, "appVersion");
        return m;
    } catch (const json::exception&) {
        Damaged();
    }
}

json ToJson(const store::SessionRecord& r) {
    json turns = json::array();
    for (const auto& t : r.turns) {
        turns.push_back({{"firstFrame", t.first_frame},
                         {"frameCount", t.frame_count},
                         {"speaker", t.speaker},
                         {"text", t.text}});
    }
    json documents = json::array();
    for (const auto& d : r.documents) {
        const store::Document& doc = d.document;
        documents.push_back({{"kind", NameOf(d.kind)},
                             {"text", doc.text},
                             {"language", doc.language},
                             {"style", doc.style},
                             {"detail", doc.detail},
                             {"generatedAt", doc.generated_at},
                             {"editedAt", doc.edited_at},
                             {"revision", doc.revision}});
    }
    return json{{"id", r.id},
                {"startedAt", r.started_at},
                {"endedAt", r.ended_at},
                {"sampleRate", r.sample_rate},
                {"deviceId", r.device_id},
                {"deviceName", r.device_name},
                {"lostFrames", r.lost_frames},
                {"turns", turns},
                {"documents", documents}};
}

store::SessionRecord RecordFromJson(const json& j) {
    try {
        store::SessionRecord r;
        r.id = Str(j, "id");
        r.started_at = Str(j, "startedAt");
        r.ended_at = Str(j, "endedAt");
        r.sample_rate = Int(j, "sampleRate");
        r.device_id = Str(j, "deviceId");
        r.device_name = Str(j, "deviceName");
        r.lost_frames = Unsigned(j, "lostFrames");
        for (const json& t : Array(j, "turns")) {
            asr::Turn turn;
            turn.first_frame = Unsigned(t, "firstFrame");
            turn.frame_count = Unsigned(t, "frameCount");
            turn.speaker = Str(t, "speaker");
            turn.text = Str(t, "text");
            r.turns.push_back(std::move(turn));
        }
        for (const json& d : Array(j, "documents")) {
            store::RecordDocument document;
            document.kind = KindOf(Str(d, "kind"));
            for (const auto& seen : r.documents) {
                if (seen.kind == document.kind) Damaged();  // one of each kind
            }
            store::Document& doc = document.document;
            doc.text = Str(d, "text");
            doc.language = Str(d, "language");
            doc.style = Str(d, "style");
            doc.detail = Str(d, "detail");
            doc.generated_at = Str(d, "generatedAt");
            doc.edited_at = Str(d, "editedAt");
            doc.revision = Signed(d, "revision", 0, std::numeric_limits<std::int64_t>::max());
            r.documents.push_back(std::move(document));
        }
        return r;
    } catch (const json::exception&) {
        Damaged();
    }
}

}  // namespace clinicavt::archive
