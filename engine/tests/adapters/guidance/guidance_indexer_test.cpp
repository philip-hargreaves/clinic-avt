#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "adapters/guidance/corpus_store.hpp"
#include "guidance_fixture.hpp"
#include "tools/corpus/indexer.hpp"

namespace clinicavt::guidance {
namespace {

constexpr const char* kFixtureDir = CLINICAVT_GUIDANCE_FIXTURE_DIR;
constexpr int kDim = 8;

// Deterministic unit vectors from the text, so a rebuild reproduces a corpus
struct FakeEmbedder : IEmbedder {
    EmbedderIdentity identity{"fx-embed-int8", "abc123", kDim, 512};
    const EmbedderIdentity& Identity() const override {
        return identity;
    }
    Embedding Embed(const std::string& text) override {
        std::uint32_t state = 2166136261u;
        for (unsigned char c : text) state = (state ^ c) * 16777619u;
        Embedding out;
        out.tokens = text.size() / 4;
        out.truncated = out.tokens > 512;
        double norm = 0;
        for (int d = 0; d < kDim; ++d) {
            state = state * 1664525u + 1013904223u;
            const float v = static_cast<float>(state >> 8) / 16777216.0f - 0.5f;
            out.vector.push_back(v);
            norm += static_cast<double>(v) * v;
        }
        for (auto& v : out.vector) v /= static_cast<float>(std::sqrt(norm));
        return out;
    }
};

using fixture::TempDir;

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string Paragraph(int words) {
    std::string out;
    for (int i = 0; i < words; ++i) out += (i ? " word" : "word");
    return out;
}

TEST(ChunksFromNiceDir, KeepsOnlyRequestedCurrentFullDocuments) {
    TempDir dir("nice");
    const nlohmann::json manifest{{"fx100", {{"status", "current"}, {"is_stub", false}}},
                                  {"fx200", {{"status", "current"}, {"is_stub", false}}},
                                  {"fx300", {{"status", "withdrawn"}}},
                                  {"fx400", {{"status", "current"}, {"is_stub", true}}},
                                  {"fx500", nlohmann::json::object()}};
    WriteText(dir.path / "manifest.json", manifest.dump());
    auto doc = [](const std::string& code) {
        const nlohmann::json rec{
            {"id", code + "-1_1_1"}, {"kind", "recommendation"}, {"text", "Offer something."}};
        const nlohmann::json chapter{{"title", "Recommendations"},
                                     {"slug", "rec"},
                                     {"recommendations", nlohmann::json::array({rec})}};
        return nlohmann::json{{"code", code},
                              {"title", "Fictional"},
                              {"source_url", "https://example.test"},
                              {"chapters", nlohmann::json::array({chapter})}};
    };
    for (const char* code : {"fx100", "fx200", "fx300", "fx400", "fx500"})
        WriteText(dir.path / "json" / (std::string(code) + ".json"), doc(code).dump());
    const auto chunks = ChunksFromNiceDir(dir.path, {"fx100", "fx300", "fx400", "fx500"});
    ASSERT_EQ(chunks.size(), 1u)
        << "fx200 not requested, fx300 withdrawn, fx400 a stub, fx500 without a status";
    EXPECT_EQ(chunks[0].id, "fx100-1_1_1");
}

TEST(IndexCorpus, BuildsACorpusTheStoreOpensWithTheEmbeddersIdentity) {
    TempDir dir("build");
    fixture::WriteMarkdown(kFixtureDir, dir.path / "docs");
    WriteText(dir.path / "docs" / "Gout.md",
              "# Fictional gout guideline\n\n1.1.1 " + Paragraph(20) + "\n\n" + Paragraph(20));
    WriteText(dir.path / "docs" / "ignored.pdf", "%PDF");
    WriteText(
        dir.path / "build.json",
        R"({"id": "fixture-2026-09", "name": "Fixture", "licence": "invented", "attribution": "none",
                  "source": "text", "text": {"dir": "docs"}})");
    const auto spec = ReadBuildSpec(dir.path / "build.json");
    EXPECT_EQ(spec.corpus.id, "fixture-2026-09");
    EXPECT_EQ(spec.text_dir, dir.path / "docs") << "paths resolve beside the spec";
    WriteText(dir.path / "bad.json",
              R"({"id": "x", "name": "x", "licence": "x", "attribution": "x", "source": "pdf"})");
    EXPECT_THROW(ReadBuildSpec(dir.path / "bad.json"), std::runtime_error);

    // One document per file, titled by its heading or else its stem
    const auto chunks = ChunksFromTextDir(spec.text_dir);
    ASSERT_EQ(chunks.size(), 41u) << "one chunk per numbered recommendation, and the gout file";
    for (const auto& chunk : chunks) {
        if (chunk.id == "gout-1") {
            EXPECT_EQ(chunk.title, "Fictional gout guideline");
            EXPECT_EQ(chunk.number, "1.1.1");
            EXPECT_EQ(chunk.url, "Gout.md");
        } else {
            EXPECT_EQ(chunk.title, chunk.id.substr(0, chunk.id.find('-'))) << chunk.id;
        }
    }

    FakeEmbedder embedder;
    std::size_t calls = 0, last_total = 0;
    const auto report = IndexCorpus(spec, embedder, dir.path / "out", "2026-09-11T00:00:00Z",
                                    "engine_tests", [&](std::size_t, std::size_t total) {
                                        ++calls;
                                        last_total = total;
                                    });
    EXPECT_EQ(report.chunks, chunks.size());
    EXPECT_EQ(calls, report.chunks);
    EXPECT_EQ(last_total, report.chunks);
    EXPECT_EQ(report.truncated, 0u);

    std::string reason;
    const auto store = CorpusStore::Open(dir.path / "out", embedder.Identity(), reason);
    ASSERT_NE(store, nullptr) << reason;
    EXPECT_EQ(store->Size(), report.chunks);
    EXPECT_EQ(store->Info().embedder_id, "fx-embed-int8");
    EXPECT_EQ(store->Info().built_at, "2026-09-11T00:00:00Z");
    const auto first = embedder.Embed(store->TextAt(0).text).vector;
    for (int d = 0; d < kDim; ++d) EXPECT_FLOAT_EQ(store->Matrix()[d], first[d]);
}

}  // namespace
}  // namespace clinicavt::guidance
