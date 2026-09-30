#include <gtest/gtest.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "adapters/storage/chunk_cipher.hpp"
#include "adapters/storage/db.hpp"
#include "adapters/storage/sqlite_session_store.hpp"
#include "adapters/storage/store_migrations.hpp"
#include "core/archive/record_rules.hpp"

namespace clinicavt::store {
namespace {

struct TempRoot {
    std::filesystem::path path;

    TempRoot() {
        path = std::filesystem::temp_directory_path() /
               ("clinicavt-schema-" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
    }

    ~TempRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path DbPath() const {
        return path / "clinicavt.db";
    }
};

std::filesystem::path SnapshotPath(std::int64_t version) {
    return std::filesystem::path(CLINICAVT_STORAGE_FIXTURE_DIR) /
           ("schema-v" + std::to_string(version) + ".txt");
}

// The table's SQL without comments, whitespace collapsed
std::string Normalised(const std::string& sql) {
    std::string out;
    for (std::size_t i = 0; i < sql.size(); ++i) {
        if (sql.compare(i, 2, "--") == 0) {
            while (i < sql.size() && sql[i] != '\n') ++i;
        }
        const char c = i < sql.size() ? sql[i] : ' ';
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

// Every CHECK expression in the order the SQL gives them
std::vector<std::string> Checks(const std::string& sql) {
    std::vector<std::string> checks;
    std::string upper = sql;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (std::size_t at = upper.find("CHECK"); at != std::string::npos;
         at = upper.find("CHECK", at + 5)) {
        std::size_t open = sql.find('(', at);
        int depth = 0;
        for (std::size_t i = open; i < sql.size(); ++i) {
            depth += sql[i] == '(' ? 1 : sql[i] == ')' ? -1 : 0;
            if (depth == 0) {
                checks.push_back(sql.substr(open + 1, i - open - 1));
                break;
            }
        }
    }
    return checks;
}

// Tables, columns, keys, indexes and checks, one per line, from the file's own schema
std::string Structure(Db& db) {
    std::ostringstream out;
    Db::Stmt tables = db.Prepare(
        "SELECT name, sql FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite_%'"
        " ORDER BY name");
    while (tables.Step()) {
        const std::string name = tables.ColumnText(0);
        const std::string sql = Normalised(tables.ColumnText(1));
        out << "table " << name << "\n";
        Db::Stmt columns = db.Prepare(("PRAGMA table_xinfo(" + name + ")").c_str());
        while (columns.Step()) {
            out << "  column " << columns.ColumnText(1) << " " << columns.ColumnText(2);
            if (columns.ColumnInt64(3) != 0) out << " not null";
            if (!columns.ColumnText(4).empty()) out << " default " << columns.ColumnText(4);
            if (columns.ColumnInt64(5) != 0) out << " key " << columns.ColumnInt64(5);
            out << "\n";
        }
        Db::Stmt keys = db.Prepare(("PRAGMA foreign_key_list(" + name + ")").c_str());
        while (keys.Step()) {
            out << "  foreign key " << keys.ColumnText(3) << " -> " << keys.ColumnText(2) << "("
                << keys.ColumnText(4) << ") on delete " << keys.ColumnText(6) << "\n";
        }
        Db::Stmt indexes = db.Prepare(("PRAGMA index_list(" + name + ")").c_str());
        while (indexes.Step()) {
            out << "  index" << (indexes.ColumnInt64(2) != 0 ? " unique" : "") << " ("
                << indexes.ColumnText(3) << ")";
            Db::Stmt parts =
                db.Prepare(("PRAGMA index_info(" + indexes.ColumnText(1) + ")").c_str());
            while (parts.Step()) out << " " << parts.ColumnText(2);
            out << "\n";
        }
        for (const std::string& check : Checks(sql)) out << "  check " << check << "\n";
        if (sql.find("WITHOUT ROWID") != std::string::npos) out << "  without rowid\n";
    }
    Db::Stmt indexes = db.Prepare(
        "SELECT name, tbl_name FROM sqlite_schema WHERE type = 'index' AND sql IS NOT NULL"
        " ORDER BY name");
    while (indexes.Step()) {
        out << "index " << indexes.ColumnText(0) << " on " << indexes.ColumnText(1) << "\n";
    }
    return out.str();
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string FreshStructure(const TempRoot& root) {
    {
        SqliteSessionStore store(root.path, std::chrono::hours(1));
    }
    Db db(root.DbPath());
    return Structure(db);
}

bool WritingSnapshot() {
    char* value = nullptr;
    const bool set = _dupenv_s(&value, nullptr, "CLINICAVT_WRITE_SCHEMA") == 0 &&
                     value != nullptr && std::string(value) == "1";
    std::free(value);
    return set;
}

// Refreshes the snapshot after a deliberate schema change, with the version bumped:
//   $env:CLINICAVT_WRITE_SCHEMA=1; engine_tests --gtest_filter=StoreSchema.WriteSnapshot
TEST(StoreSchema, WriteSnapshot) {
    if (!WritingSnapshot()) {
        GTEST_SKIP() << "writes the snapshot only with CLINICAVT_WRITE_SCHEMA=1";
    }
    TempRoot root;
    ASSERT_FALSE(std::filesystem::exists(SnapshotPath(kSchemaVersion)))
        << "a released version's snapshot is never rewritten";
    std::ofstream(SnapshotPath(kSchemaVersion), std::ios::binary) << FreshStructure(root);
}

// Editing clinicavt.sql without a new version and an upgrade step fails here
TEST(StoreSchema, TheCreatedSchemaMatchesTheSnapshotForItsVersion) {
    TempRoot root;
    const auto snapshot = SnapshotPath(kSchemaVersion);
    ASSERT_TRUE(std::filesystem::exists(snapshot)) << "no snapshot for " << kSchemaVersion;
    EXPECT_EQ(FreshStructure(root), ReadText(snapshot));
}

// The released version 6 schema
constexpr const char* kSchemaV6 = R"sql(
CREATE TABLE sessions (
    id           TEXT    PRIMARY KEY,
    started_at   TEXT    NOT NULL,
    ended_at     TEXT,
    state        TEXT    NOT NULL
                 CHECK (state IN ('recording', 'finalised')),
    sample_rate  INTEGER NOT NULL,
    device_id    TEXT,
    device_name  TEXT,
    lost_frames  INTEGER NOT NULL DEFAULT 0,
    retain       INTEGER NOT NULL DEFAULT 1,
    demo         INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE session_keys (
    session_id   TEXT    PRIMARY KEY REFERENCES sessions (id) ON DELETE CASCADE,
    wrapped      BLOB    NOT NULL
);
CREATE TABLE chunks (
    session_id   TEXT    NOT NULL REFERENCES sessions (id) ON DELETE CASCADE,
    seq          INTEGER NOT NULL,
    first_frame  INTEGER NOT NULL,
    frame_count  INTEGER NOT NULL,
    lost_before  INTEGER NOT NULL,
    payload      BLOB    NOT NULL,
    PRIMARY KEY (session_id, seq)
);
CREATE TABLE turns (
    session_id   TEXT    NOT NULL REFERENCES sessions (id) ON DELETE CASCADE,
    seq          INTEGER NOT NULL,
    first_frame  INTEGER NOT NULL,
    frame_count  INTEGER NOT NULL,
    payload      BLOB    NOT NULL,
    PRIMARY KEY (session_id, seq)
);
CREATE TABLE documents (
    session_id   TEXT    NOT NULL REFERENCES sessions (id) ON DELETE CASCADE,
    kind         TEXT    NOT NULL
                 CHECK (kind IN ('note', 'patient', 'translation', 'label',
                                 'summary', 'reflection', 'guidance')),
    seq          INTEGER NOT NULL DEFAULT 0,
    language     TEXT    NOT NULL,
    payload      BLOB    NOT NULL,
    generated_at TEXT,
    edited_at    TEXT,
    PRIMARY KEY (session_id, kind)
);
CREATE TABLE note_options (
    session_id   TEXT    PRIMARY KEY,
    kind         TEXT    NOT NULL DEFAULT 'note' CHECK (kind = 'note'),
    style        TEXT    NOT NULL,
    detail       TEXT    NOT NULL,
    FOREIGN KEY (session_id, kind) REFERENCES documents (session_id, kind) ON DELETE CASCADE
);
)sql";

std::span<const std::uint8_t> Bytes(const std::string& text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::span<const std::uint8_t> Bytes(const std::vector<float>& frames) {
    return {reinterpret_cast<const std::uint8_t*>(frames.data()), frames.size() * sizeof(float)};
}

// Writes rows through the version 6 shapes, sealed as that build sealed them
class V6Store {
   public:
    explicit V6Store(const TempRoot& root) {
        std::filesystem::create_directories(root.path);
        db_.emplace(root.DbPath());
        db_->Exec("PRAGMA auto_vacuum=INCREMENTAL");
        db_->Exec("VACUUM");
        Db::Transaction txn(*db_);
        db_->Exec(kSchemaV6);
        db_->SetApplicationId(kApplicationId);
        db_->SetUserVersion(6);
        txn.Commit();
    }

    Db& Database() {
        return *db_;
    }

    void AddSession(const std::string& id, const char* state, const char* ended_at, int retain,
                    int demo, std::int64_t lost_frames) {
        Db::Stmt insert = db_->Prepare(
            "INSERT INTO sessions(id, started_at, ended_at, state, sample_rate, device_id,"
            " device_name, lost_frames, retain, demo)"
            " VALUES(?, '2026-09-20T09:00:00Z', ?, ?, 16000, 'mic-1', 'Desk mic', ?, ?, ?)");
        insert.BindText(1, id);
        insert.BindTextOrNull(2, ended_at != nullptr ? ended_at : "");
        insert.BindText(3, state);
        insert.BindInt64(4, lost_frames);
        insert.BindInt64(5, retain);
        insert.BindInt64(6, demo);
        insert.Step();
        auto cipher = ChunkCipher::Generate();
        Db::Stmt key = db_->Prepare("INSERT INTO session_keys(session_id, wrapped) VALUES(?, ?)");
        key.BindText(1, id);
        key.BindBlob(2, cipher.Wrapped());
        key.Step();
        keys_.emplace(id, std::move(cipher));
    }

    void AddTurn(const std::string& id, std::int64_t seq, const std::string& speaker,
                 const std::string& text) {
        const std::string content = nlohmann::json{{"speaker", speaker}, {"text", text}}.dump();
        Db::Stmt insert = db_->Prepare(
            "INSERT INTO turns(session_id, seq, first_frame, frame_count, payload)"
            " VALUES(?, ?, ?, 16000, ?)");
        insert.BindText(1, id);
        insert.BindInt64(2, seq);
        insert.BindInt64(3, seq * 16000);
        insert.BindBlob(4, keys_.at(id).Seal(Domain::kTurns, id, static_cast<std::uint64_t>(seq),
                                             Bytes(content)));
        insert.Step();
    }

    void AddChunk(const std::string& id, std::int64_t seq, std::int64_t lost_before,
                  const std::vector<float>& frames) {
        Db::Stmt insert = db_->Prepare(
            "INSERT INTO chunks(session_id, seq, first_frame, frame_count, lost_before, payload)"
            " VALUES(?, ?, ?, ?, ?, ?)");
        insert.BindText(1, id);
        insert.BindInt64(2, seq);
        insert.BindInt64(3, seq * static_cast<std::int64_t>(frames.size()));
        insert.BindInt64(4, static_cast<std::int64_t>(frames.size()));
        insert.BindInt64(5, lost_before);
        insert.BindBlob(6, keys_.at(id).Seal(Domain::kAudio, id, static_cast<std::uint64_t>(seq),
                                             Bytes(frames)));
        insert.Step();
    }

    void AddDocument(const std::string& id, const char* kind, Domain domain, std::int64_t seq,
                     const std::string& text, const char* language = "en") {
        Db::Stmt insert = db_->Prepare(
            "INSERT INTO documents(session_id, kind, seq, language, payload, generated_at)"
            " VALUES(?, ?, ?, ?, ?, '2026-09-20T09:20:00Z')");
        insert.BindText(1, id);
        insert.BindText(2, kind);
        insert.BindInt64(3, seq);
        insert.BindText(4, language);
        insert.BindBlob(
            5, keys_.at(id).Seal(domain, id, static_cast<std::uint64_t>(seq), Bytes(text)));
        insert.Step();
    }

    void AddNoteOptions(const std::string& id, const char* style, const char* detail) {
        Db::Stmt insert =
            db_->Prepare("INSERT INTO note_options(session_id, style, detail) VALUES(?, ?, ?)");
        insert.BindText(1, id);
        insert.BindText(2, style);
        insert.BindText(3, detail);
        insert.Step();
    }

    void Close() {
        db_.reset();
    }

   private:
    std::optional<Db> db_;
    std::map<std::string, ChunkCipher> keys_;
};

const std::string kReal = std::string(31, 'a') + "1";
const std::string kSample = std::string(31, 'b') + "2";
const std::string kCrashed = std::string(31, 'c') + "3";

std::vector<float> Tone(std::size_t frames, float offset) {
    std::vector<float> audio(frames);
    for (std::size_t i = 0; i < frames; ++i) audio[i] = offset + static_cast<float>(i) / 1e5f;
    return audio;
}

// A real consultation, a sample and a crashed recording, with documents at sequence 0 as the
// old 5 to 6 upgrade left them and one that no longer opens
void FillV6(V6Store& v6) {
    v6.AddSession(kReal, "finalised", "2026-09-20T09:15:00Z", 1, 0, 7);
    v6.AddTurn(kReal, 0, "doctor", "what brings you in");
    v6.AddTurn(kReal, 1, "patient", "my elbow is swollen");
    v6.AddDocument(kReal, "note", Domain::kNote, 0, "Swollen left elbow.");
    v6.AddNoteOptions(kReal, "soap", "standard");
    v6.AddDocument(kReal, "patient", Domain::kPatient, 4611686018427387901,
                   "Your elbow is swollen.");
    v6.AddDocument(kReal, "translation", Domain::kTranslation, 11, "Aapki kohni", "Hindi");
    v6.AddDocument(kReal, "label", Domain::kLabel, 5, "Elbow swelling");
    v6.AddDocument(kReal, "reflection", Domain::kReflection, 9, R"({"happened":"x"})");
    v6.AddDocument(kReal, "summary", Domain::kSummary, 0, "A patient in their forties.");
    {
        Db::Stmt corrupt = v6.Database().Prepare(
            "INSERT INTO documents(session_id, kind, seq, language, payload)"
            " VALUES(?, 'guidance', 0, 'en', randomblob(64))");
        corrupt.BindText(1, kReal);
        corrupt.Step();
    }

    v6.AddSession(kSample, "finalised", "2026-09-20T09:40:00Z", 1, 1, 0);
    v6.AddTurn(kSample, 0, "doctor", "a seeded consultation");
    v6.AddDocument(kSample, "label", Domain::kLabel, 0, "Sample: knee pain");
    v6.AddDocument(kSample, "note", Domain::kNote, 3, "Knee pain.");
    v6.AddNoteOptions(kSample, "prose", "detailed");

    v6.AddSession(kCrashed, "recording", nullptr, 0, 0, 0);
    v6.AddChunk(kCrashed, 0, 0, Tone(1600, 0.1f));
    v6.AddChunk(kCrashed, 1, 160, Tone(1600, 0.2f));
}

const RecordDocument* Find(const SessionRecord& record, DocumentKind kind) {
    for (const auto& entry : record.documents) {
        if (entry.kind == kind) return &entry;
    }
    return nullptr;
}

TEST(StoreMigration, AVersion6StoreUpgradesWithEveryRowReadableAndSound) {
    TempRoot root;
    {
        V6Store v6(root);
        FillV6(v6);
    }
    TempRoot fresh;
    const std::string expected_structure = FreshStructure(fresh);

    SqliteSessionStore store(root.path, std::chrono::hours(1));
    const auto listed = store.ListSessions();
    ASSERT_EQ(listed.size(), 3u);
    std::map<std::string, SessionSummary> by_id;
    for (const auto& summary : listed) by_id[summary.id] = summary;
    EXPECT_EQ(by_id[kReal].label, "Elbow swelling");
    EXPECT_FALSE(by_id[kReal].sample);
    EXPECT_TRUE(by_id[kReal].has_reflection);
    EXPECT_FALSE(by_id[kReal].cleared);
    EXPECT_EQ(by_id[kSample].label, "Sample: knee pain") << "a label resealed from sequence 0";
    EXPECT_TRUE(by_id[kSample].sample);
    EXPECT_EQ(by_id[kCrashed].state, SessionState::kRecording);

    const SessionRecord real = store.ReadRecord(kReal);
    EXPECT_TRUE(archive::ValidRecord(real)) << "every row backs up";
    EXPECT_EQ(real.device_name, "Desk mic");
    EXPECT_EQ(real.lost_frames, 7u);
    ASSERT_EQ(real.turns.size(), 2u);
    EXPECT_EQ(real.turns[1].speaker, "patient");
    EXPECT_EQ(real.turns[1].text, "my elbow is swollen");
    const RecordDocument* note = Find(real, DocumentKind::kNote);
    ASSERT_NE(note, nullptr);
    EXPECT_EQ(note->document.text, "Swollen left elbow.");
    EXPECT_EQ(note->document.style, "soap");
    EXPECT_EQ(note->document.detail, "concise") << "standard was renamed";
    EXPECT_GT(note->document.revision, 0) << "resealed away from sequence 0";
    EXPECT_EQ(Find(real, DocumentKind::kPatient)->document.revision, 4611686018427387901);
    EXPECT_EQ(Find(real, DocumentKind::kTranslation)->document.language, "Hindi");
    EXPECT_EQ(Find(real, DocumentKind::kLabel)->document.revision, 5);
    EXPECT_EQ(Find(real, DocumentKind::kReflection)->document.text, R"({"happened":"x"})");
    EXPECT_EQ(Find(real, DocumentKind::kSummary)->document.text, "A patient in their forties.");
    EXPECT_EQ(Find(real, DocumentKind::kGuidance), nullptr) << "the unreadable row is gone";
    EXPECT_TRUE(Find(real, DocumentKind::kPatient)->document.style.empty());

    const Document sample_note = store.ReadDocument(kSample, DocumentKind::kNote);
    EXPECT_EQ(sample_note.text, "Knee pain.");
    EXPECT_EQ(sample_note.detail, "detailed");
    EXPECT_EQ(sample_note.revision, 3);
    EXPECT_EQ(store.ReadTurns(kSample)[0].text, "a seeded consultation");

    std::vector<float> audio = Tone(1600, 0.1f);
    const auto second = Tone(1600, 0.2f);
    audio.insert(audio.end(), second.begin(), second.end());
    EXPECT_EQ(store.ReadAudio(kCrashed), audio);

    Db db(root.DbPath());
    EXPECT_EQ(db.UserVersion(), kSchemaVersion);
    EXPECT_EQ(db.QueryInt64("SELECT count(*) FROM documents WHERE revision <= 0"), 0);
    EXPECT_EQ(db.QueryInt64("SELECT dropped_before FROM audio_chunks WHERE sequence = 1"), 160);
    EXPECT_EQ(db.QueryInt64("SELECT saved FROM consultations WHERE state = 'recording'"), 0);
    Db::Stmt dangling = db.Prepare("PRAGMA foreign_key_check");
    EXPECT_FALSE(dangling.Step());
    Db::Stmt integrity = db.Prepare("PRAGMA integrity_check");
    ASSERT_TRUE(integrity.Step());
    EXPECT_EQ(integrity.ColumnText(0), "ok");
    EXPECT_EQ(Structure(db), expected_structure) << "an upgraded store matches a new one";
}

// The step and its version stamp commit together, so a step that fails leaves version 6 as it
// was and the next open tries again
TEST(StoreMigration, AFailedStepLeavesTheVersion6StoreUntouched) {
    TempRoot root;
    {
        V6Store v6(root);
        FillV6(v6);
        v6.Database().Exec("PRAGMA foreign_keys=OFF");
        v6.Database().Exec(
            "INSERT INTO turns(session_id, seq, first_frame, frame_count, payload)"
            " VALUES('ffffffffffffffffffffffffffffffff', 0, 0, 1, x'00')");
    }
    EXPECT_THROW(SqliteSessionStore(root.path, std::chrono::hours(1)), StoreError);
    {
        Db db(root.DbPath());
        EXPECT_EQ(db.UserVersion(), 6);
        EXPECT_EQ(db.QueryInt64("SELECT count(*) FROM sessions"), 3);
        EXPECT_EQ(db.QueryInt64("SELECT count(*) FROM documents WHERE seq = 0"), 4)
            << "nothing was resealed";
        EXPECT_EQ(db.QueryInt64("SELECT count(*) FROM note_options"), 2);
        EXPECT_EQ(db.QueryInt64("SELECT count(*) FROM sqlite_schema WHERE name LIKE '%_v6'"), 0);
        db.Exec("DELETE FROM turns WHERE session_id = 'ffffffffffffffffffffffffffffffff'");
    }
    SqliteSessionStore store(root.path, std::chrono::hours(1));
    EXPECT_EQ(store.ListSessions().size(), 3u);
    EXPECT_EQ(store.ReadDocument(kReal, DocumentKind::kNote).text, "Swollen left elbow.");
}

}  // namespace
}  // namespace clinicavt::store
