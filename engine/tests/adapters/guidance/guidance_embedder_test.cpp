#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapters/guidance/embedder.hpp"
#include "adapters/guidance/retriever.hpp"
#include "adapters/models/model_store.hpp"
#include "guidance_fixture.hpp"

namespace clinicavt::guidance {
namespace {

constexpr const char* kFixtureDir = CLINICAVT_GUIDANCE_FIXTURE_DIR;

const models::ModelStore& Store() {
    static const models::ModelStore kStore{std::filesystem::path(CLINICAVT_MODELS_DIR)};
    return kStore;
}

Embedder& StagedEmbedder() {
    static const auto kEmbedder = Embedder::Load(Store());
    return *kEmbedder;
}

// Non-owning, so an owning retriever can use the one loaded model
struct Borrowed : IEmbedder {
    IEmbedder& inner;
    explicit Borrowed(IEmbedder& embedder) : inner(embedder) {}
    const EmbedderIdentity& Identity() const override {
        return inner.Identity();
    }
    Embedding Embed(const std::string& text) override {
        return inner.Embed(text);
    }
};

double Cosine(const std::vector<float>& a, const std::vector<float>& b) {
    double dot = 0;
    for (std::size_t i = 0; i < a.size(); ++i) dot += static_cast<double>(a[i]) * b[i];
    return dot;
}

std::string Join(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) out += (out.empty() ? "" : ", ") + item;
    return out;
}

// The staged model loads through the guards and matches the Python harness
// on the fixture texts it embedded
TEST(GuidanceEmbedder, LoadsAndMatchesTheHarnessEmbeddings) {
    auto& embedder = StagedEmbedder();
    const auto& identity = embedder.Identity();
    EXPECT_EQ(identity.id, "gte-large-int8");
    EXPECT_EQ(identity.dim, 1024);
    EXPECT_EQ(identity.max_tokens, 512);
    EXPECT_EQ(identity.rev,
              Store().Resolve("embedding", "default").file_hashes.at("openvino_model.bin"));

    std::ifstream in(std::filesystem::path(kFixtureDir) / "embeddings.json");
    ASSERT_TRUE(in.is_open());
    const auto fixture = nlohmann::json::parse(in);
    ASSERT_EQ(fixture.at("model"), "gte-large-int8");
    const auto texts = fixture.at("texts").get<std::vector<std::string>>();
    const auto vectors = fixture.at("vectors").get<std::vector<std::vector<float>>>();
    for (std::size_t i = 0; i < texts.size(); ++i) {
        const auto ours = embedder.Embed(texts[i]);
        ASSERT_EQ(ours.vector.size(), vectors[i].size());
        EXPECT_GE(Cosine(ours.vector, vectors[i]), 0.999) << texts[i];
        EXPECT_NEAR(Cosine(ours.vector, ours.vector), 1.0, 1e-3) << "unit length";
        EXPECT_FALSE(ours.truncated);
        EXPECT_GT(ours.tokens, 5u);
    }

    std::string long_text;
    for (int i = 0; i < 1500; ++i) long_text += "symptom ";
    const auto out = embedder.Embed(long_text);
    EXPECT_TRUE(out.truncated) << "a text past the model's window says so";
    EXPECT_GT(out.tokens, 512u);
    EXPECT_EQ(out.vector.size(), 1024u);
}

// The retriever over the staged model and the fixture corpus: each fixture
// note surfaces its guideline first and one of its expected recommendations in
// the top three, and none of the guidelines it must avoid. The non-clinical text is
// refused at the shipped floor. Ordering is asserted without the floor. What
// the floor does with each note is printed, since the fixture is invented text
TEST(GuidanceEmbedder, SearchesTheFixtureNotes) {
    const fixture::TempDir temp("real");
    const auto& dir = temp.path;
    auto& embedder = StagedEmbedder();
    fixture::Build(dir / "corpora" / "fixture", "fixture", embedder, fixture::Chunks(kFixtureDir));
    {
        auto lend = [&]() -> std::unique_ptr<IEmbedder> {
            return std::make_unique<Borrowed>(embedder);
        };
        RetrieverOptions unfloored;
        unfloored.floor = 0;
        Retriever ordering(lend, dir / "corpora", unfloored);
        Retriever shipped(lend, dir / "corpora");
        for (const auto& note : fixture::Notes(kFixtureDir)) {
            const auto results = ordering.Search(note.text, 3, SearchMode::kNote);
            const auto floored = shipped.Search(note.text, 3, SearchMode::kNote);
            std::vector<std::string> ids;
            for (const auto& r : results.shown) ids.push_back(r.chunk_id);
            if (note.expected.empty()) {
                EXPECT_TRUE(floored.abstained) << note.id << ": " << Join(ids);
                continue;
            }
            ASSERT_FALSE(ids.empty()) << note.id;
            const auto code = note.expected[0].substr(0, note.expected[0].find('-'));
            EXPECT_EQ(results.shown[0].code, code) << note.id << ": " << Join(ids);
            bool any = false;
            for (const auto& id : note.expected) any |= std::count(ids.begin(), ids.end(), id) > 0;
            EXPECT_TRUE(any) << note.id << ": " << Join(ids);
            for (const auto& banned : note.must_not) {
                for (const auto& r : results.shown) EXPECT_NE(r.code, banned) << note.id;
            }
            std::printf("  %s: top cosine %.3f, %zu of %d shown at the shipped floor\n",
                        note.id.c_str(), results.shown[0].score, floored.shown.size(),
                        floored.considered);
        }
    }
}

}  // namespace
}  // namespace clinicavt::guidance
