#include "adapters/diarisation/speaker_embedder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "dev_wav.hpp"

namespace clinicavt::diar {
namespace {

// The full stage against the research reference: the engine fbank and runtime on
// raw audio must reproduce the fixture embeddings (research fbank + research
// runtime). A failure here with fbank parity green points at the glue or the IR
constexpr const char* kFixtureDir = CLINICAVT_DIAR_FIXTURE_DIR;

float Dot(const std::vector<float>& a, const std::vector<float>& b) {
    float dot = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) dot += a[i] * b[i];
    return dot;
}

TEST(SpeakerEmbedder, ReproducesTheResearchEmbeddingsFromRawAudio) {
    const models::ModelStore store{std::filesystem::path(CLINICAVT_MODELS_DIR)};
    models::OvRuntime runtime;
    EXPECT_EQ(runtime.Load(store, "diarisation", "default", "model.xml").device, "CPU")
        << "the embedder's researched placement";

    std::ifstream in(std::filesystem::path(kFixtureDir) / "fixtures.json");
    ASSERT_TRUE(in.is_open()) << "missing diarisation fixtures";
    const auto meta = nlohmann::json::parse(in);
    // The fixture names a wav that is not in the repo
    if (!std::filesystem::exists(meta.at("wav").get<std::string>())) {
        GTEST_SKIP() << "research corpus not mounted";
    }
    const auto audio = LoadDevWav(meta.at("wav"));
    SpeakerEmbedder embedder(store, runtime);

    std::vector<std::string> roles;
    std::vector<std::vector<float>> embeddings;
    for (const auto& entry : meta.at("slices")) {
        const auto first = static_cast<std::size_t>(entry.at("start_s").get<double>() * 16000.0);
        const auto last = static_cast<std::size_t>(entry.at("end_s").get<double>() * 16000.0);
        auto embedding = embedder.Embed(std::span<const float>(audio).subspan(first, last - first));
        ASSERT_EQ(embedding.size(), kEmbeddingDims);
        EXPECT_GE(Dot(embedding, entry.at("embedding").get<std::vector<float>>()), 0.999f)
            << entry.at("file").get<std::string>();
        roles.push_back(entry.at("role"));
        embeddings.push_back(std::move(embedding));
    }

    // Speaker identity must dominate the embedding space. Compare means, since a
    // mixed track leaks backchannel into some slices
    float same = 0.0f, cross = 0.0f;
    int same_n = 0, cross_n = 0;
    for (std::size_t a = 0; a < embeddings.size(); ++a) {
        for (std::size_t b = a + 1; b < embeddings.size(); ++b) {
            const float cos = Dot(embeddings[a], embeddings[b]);
            if (roles[a] == roles[b]) {
                same += cos;
                ++same_n;
            } else {
                cross += cos;
                ++cross_n;
            }
        }
    }
    ASSERT_GT(same_n, 0);
    ASSERT_GT(cross_n, 0);
    same /= static_cast<float>(same_n);
    cross /= static_cast<float>(cross_n);
    EXPECT_GT(same, cross + 0.15f) << "same-role mean " << same << ", cross-role mean " << cross;
}

}  // namespace
}  // namespace clinicavt::diar
