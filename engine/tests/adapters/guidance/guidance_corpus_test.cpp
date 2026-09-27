#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "adapters/guidance/corpus_store.hpp"
#include "adapters/storage/db.hpp"
#include "adapters/system/sha256.hpp"
#include "guidance_fixture.hpp"
#include "tools/corpus/corpus_builder.hpp"

namespace clinicavt::guidance {
namespace {

namespace fs = std::filesystem;

constexpr const char* kFixtureDir = CLINICAVT_GUIDANCE_FIXTURE_DIR;
constexpr int kDim = 8;

// Deterministic unit vectors, one per chunk
std::vector<float> FixtureVectors(std::size_t rows) {
    std::vector<float> out(rows * kDim);
    std::uint32_t state = 12345;
    for (std::size_t r = 0; r < rows; ++r) {
        double norm = 0;
        for (int d = 0; d < kDim; ++d) {
            state = state * 1664525u + 1013904223u;
            const float v = static_cast<float>(state >> 8) / 16777216.0f - 0.5f;
            out[r * kDim + d] = v;
            norm += double(v) * v;
        }
        for (int d = 0; d < kDim; ++d) out[r * kDim + d] /= static_cast<float>(std::sqrt(norm));
    }
    return out;
}

EmbedderIdentity Embedder() {
    return {"fx-embed-int8", "abc123", kDim, 512};
}

CorpusSpec Spec() {
    CorpusSpec spec;
    spec.id = "fixture-2026-09-10";
    spec.name = "Fixture guidance corpus";
    spec.licence = "invented";
    spec.attribution = "none";
    spec.source = "text";
    spec.embedder = Embedder();
    spec.built_at = "2026-09-10T00:00:00Z";
    spec.builder = "engine_tests";
    return spec;
}

using fixture::TempDir;

void Build(const fs::path& dir) {
    const auto chunks = fixture::Chunks(kFixtureDir);
    BuildCorpus(dir, Spec(), chunks, FixtureVectors(chunks.size()));
}

void EditManifest(const fs::path& dir, const char* key, const nlohmann::json& value) {
    const auto manifest_path = dir / kManifestFile;
    std::ifstream in(manifest_path);
    auto manifest = nlohmann::json::parse(in);
    in.close();
    manifest[key] = value;
    std::ofstream out(manifest_path, std::ios::trunc);
    out << manifest.dump(2);
}

// Edits corpus.db by hand, then makes the manifest agree again so a deeper guard is reached
void Sql(const fs::path& dir, const char* sql) {
    {
        store::Db db(dir / kCorpusFile, store::Db::Mode::kBuild);
        db.Exec(sql);
    }
    const auto file = dir / kCorpusFile;
    EditManifest(dir, "bytes", fs::file_size(file));
    EditManifest(dir, "sha256", system::Sha256File(file));
}

// The invented corpus and notes every guidance test builds on, checked on CI
// since the notes are otherwise read only by the opt-in model test
TEST(GuidanceFixture, TheCorpusAndNotesAreConsistent) {
    const auto chunks = fixture::Chunks(kFixtureDir);
    ASSERT_GE(chunks.size(), 40u);
    const std::regex shape("fx[0-9]{3}-[0-9]+(_[0-9]+)+");
    std::set<std::string> ids;
    for (const auto& chunk : chunks) {
        EXPECT_TRUE(std::regex_match(chunk.id, shape)) << chunk.id;
        EXPECT_EQ(chunk.id.substr(0, chunk.id.find('-')), chunk.code) << chunk.id;
        EXPECT_TRUE(ids.insert(chunk.id).second) << "duplicate " << chunk.id;
        EXPECT_FALSE(chunk.text.empty()) << chunk.id;
    }
    bool negated = false, non_clinical = false;
    for (const auto& note : fixture::Notes(kFixtureDir)) {
        for (const auto& id : note.expected) EXPECT_TRUE(ids.contains(id)) << note.id << ": " << id;
        negated |= !note.must_not.empty();
        non_clinical |= note.expected.empty();
    }
    EXPECT_TRUE(negated) << "a note with a negated finding";
    EXPECT_TRUE(non_clinical) << "a non-clinical text that retrieves nothing";
}

TEST(CorpusStore, ABuiltCorpusOpensReadOnlyWithItsMatrixInOrdinalOrder) {
    TempDir root("corpus-build");
    // Characters the store's file URI must escape
    const auto dir = root.path / fs::path(u8"Program Files é 100%");
    Build(dir);
    EXPECT_FALSE(fs::exists(dir / "corpus.db.tmp"));
    EXPECT_FALSE(fs::exists(dir / "corpus.db-wal"));
    EXPECT_FALSE(fs::exists(dir / "corpus.db-shm"));

    // A rollback journal and the corpus page size are what let it open read-only
    {
        std::ifstream in(dir / kCorpusFile, std::ios::binary);
        unsigned char header[100];
        in.read(reinterpret_cast<char*>(header), sizeof header);
        EXPECT_EQ(header[18], 1) << "file format 1: rollback journal, not WAL";
        EXPECT_EQ(header[19], 1);
        EXPECT_EQ((header[16] << 8) | header[17], 1) << "page size 65536 is stored as 1";
    }

    // Installed under Program Files, the files cannot be written
    for (const char* name : {kCorpusFile, kManifestFile}) {
        SetFileAttributesW((dir / name).c_str(), FILE_ATTRIBUTE_READONLY);
    }
    std::string reason;
    const auto store = CorpusStore::Open(dir, Embedder(), reason);
    ASSERT_NE(store, nullptr) << reason;
    EXPECT_TRUE(reason.empty());
    const auto chunks = fixture::Chunks(kFixtureDir);
    const auto vectors = FixtureVectors(chunks.size());
    ASSERT_EQ(store->Size(), chunks.size());
    EXPECT_EQ(store->Dim(), kDim);
    for (std::size_t i = 0; i < vectors.size(); ++i) EXPECT_EQ(store->Matrix()[i], vectors[i]);
    EXPECT_EQ(store->CiteAt(0).chunk_id, chunks[0].id);
    EXPECT_EQ(store->CiteAt(0).section, chunks[0].section);
    EXPECT_EQ(store->CiteAt(chunks.size() - 1).chunk_id, chunks.back().id);
    EXPECT_EQ(store->TextAt(3).text, chunks[3].text);
    EXPECT_EQ(store->TextAt(3).url, chunks[3].url);
    EXPECT_EQ(store->Info().id, "fixture-2026-09-10");
    EXPECT_EQ(store->Info().chunk_count, static_cast<std::int64_t>(chunks.size()));
    EXPECT_EQ(store->Info().sha256, system::Sha256File(dir / kCorpusFile));
    EXPECT_FALSE(fs::exists(dir / "corpus.db-wal")) << "read-only open writes nothing";
}

TEST(CorpusStore, RefusesADamagedOrMismatchedCorpusWithAReason) {
    using Damage = std::function<void(const fs::path&)>;
    const auto manifest = [](const char* key, nlohmann::json value) -> Damage {
        return [=](const fs::path& dir) { EditManifest(dir, key, value); };
    };
    const auto sql = [](const char* statement) -> Damage {
        return [=](const fs::path& dir) { Sql(dir, statement); };
    };
    const auto rev = [](EmbedderIdentity& e) { e.rev = "def456"; };
    const auto tokens = [](EmbedderIdentity& e) { e.max_tokens = 256; };
    struct Case {
        const char* name;
        Damage damage;
        std::function<void(EmbedderIdentity&)> staged;
        const char* reason;
        bool exact = false;  // the reason is shown as is, so these pin it whole
    };
    const Case cases[] = {
        {"hash", manifest("sha256", std::string(64, '0')), {}, "hash"},
        {"meta id", manifest("id", "other"), {}, "differs"},
        {"meta dim", manifest("dim", kDim + 1), {}, "differs"},
        {"meta embedder", manifest("embedder_id", "other"), {}, "differs"},
        {"meta chunks", manifest("chunks", 3), {}, "differs"},
        {"embedder rev", {}, rev, "staged embedder"},
        {"embedder max tokens", {}, tokens, "max tokens"},
        {"shard length",
         sql("PRAGMA ignore_check_constraints=ON; UPDATE guidance_vectors SET data = "
             "substr(data, 5) WHERE shard = 0"),
         {},
         "length"},
        {"shard gap",
         sql("UPDATE guidance_vectors SET first_ord = first_ord + 1 WHERE shard = 0"),
         {},
         "contiguous"},
        // The first float becomes 2.0, so the first vector is no longer unit length
        {"shard norm",
         sql("UPDATE guidance_vectors SET data = unhex('00000040' || substr(hex(data), 9)) "
             "WHERE shard = 0"),
         {},
         "unit length"},
        {"newer format", sql("PRAGMA user_version=2"), {}, "format"},
        {"foreign file", sql("PRAGMA application_id=0"), {}, "not a corpus"},
        {"WAL mode", sql("PRAGMA journal_mode=WAL"), {}, "WAL"},
        {"missing manifest",
         [](const fs::path& dir) { fs::remove(dir / kManifestFile); },
         {},
         "no manifest.json",
         true},
        {"broken manifest",
         [](const fs::path& dir) {
             std::ofstream(dir / kManifestFile, std::ios::trunc) << "{ not json";
         },
         {},
         "manifest.json does not parse",
         true},
    };
    TempDir root("corpus-refusals");
    int row = 0;
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        const auto dir = root.path / std::to_string(row++);
        Build(dir);
        if (c.damage) c.damage(dir);
        auto staged = Embedder();
        if (c.staged) c.staged(staged);
        std::string reason;
        EXPECT_EQ(CorpusStore::Open(dir, staged, reason), nullptr);
        if (c.exact) {
            EXPECT_EQ(reason, c.reason);
        } else {
            EXPECT_NE(reason.find(c.reason), std::string::npos) << reason;
        }
    }
}

TEST(CorpusBuilder, RefusesBadInputAndLeavesAnExistingCorpusIntact) {
    TempDir dir("corpus-atomic");
    Build(dir.path);
    const auto before = system::Sha256File(dir.path / kCorpusFile);
    auto chunks = fixture::Chunks(kFixtureDir);
    auto vectors = FixtureVectors(chunks.size());
    vectors[0] *= 2.0f;
    EXPECT_THROW(BuildCorpus(dir.path, Spec(), chunks, vectors), std::invalid_argument);
    vectors = FixtureVectors(chunks.size());
    vectors.pop_back();
    EXPECT_THROW(BuildCorpus(dir.path, Spec(), chunks, vectors), std::invalid_argument);
    chunks[0].text.clear();
    EXPECT_THROW(BuildCorpus(dir.path, Spec(), chunks, FixtureVectors(chunks.size())),
                 std::invalid_argument);
    EXPECT_EQ(system::Sha256File(dir.path / kCorpusFile), before);
    EXPECT_FALSE(fs::exists(dir.path / "corpus.db.tmp"));
    std::string reason;
    EXPECT_NE(CorpusStore::Open(dir.path, Embedder(), reason), nullptr) << reason;
}

}  // namespace
}  // namespace clinicavt::guidance
