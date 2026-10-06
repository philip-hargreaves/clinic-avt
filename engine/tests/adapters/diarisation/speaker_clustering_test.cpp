#include "adapters/diarisation/speaker_clustering.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace clinicavt::diar {
namespace {

constexpr std::uint64_t kLong = kFitMinFrames;
constexpr std::uint64_t kShort = 8000;

// A unit vector near one of three well-separated directions, with a small
// deterministic offset so no two members are identical
std::vector<float> Voice(int direction, int member) {
    std::vector<float> v(8, 0.0f);
    v[static_cast<std::size_t>(direction)] = 1.0f;
    v[7] = 0.05f * static_cast<float>(member + 1);
    float norm = 0.0f;
    for (const float x : v) norm += x * x;
    norm = std::sqrt(norm);
    for (float& x : v) x /= norm;
    return v;
}

// Four slices per voice of the given length, voice by voice
void AddVoices(int voices, std::uint64_t duration, int first_voice,
               std::vector<std::vector<float>>& embeddings, std::vector<std::uint64_t>& durations) {
    for (int voice = first_voice; voice < first_voice + voices; ++voice) {
        for (int i = 0; i < 4; ++i) {
            embeddings.push_back(Voice(voice, i));
            durations.push_back(duration);
        }
    }
}

TEST(SpeakerClustering, TwoVoicesAreFoundAndEverySliceIsLabelled) {
    std::vector<std::vector<float>> embeddings;
    std::vector<std::uint64_t> durations;
    AddVoices(2, kLong, 0, embeddings, durations);
    embeddings.push_back(Voice(0, 9));  // short slice, same voice as the first pile
    durations.push_back(kShort);

    const auto result = ClusterSpeakers(embeddings, durations);
    EXPECT_EQ(result.count, 2);
    for (int i = 1; i < 4; ++i) EXPECT_EQ(result.labels[i], result.labels[0]);
    for (int i = 5; i < 8; ++i) EXPECT_EQ(result.labels[i], result.labels[4]);
    EXPECT_NE(result.labels[0], result.labels[4]);
    EXPECT_EQ(result.labels[8], result.labels[0]) << "a short slice joins its nearest centroid";

    ASSERT_EQ(result.centroids.size(), 2u);
    for (const auto& centroid : result.centroids) {
        double norm = 0.0;
        for (const float x : centroid) norm += static_cast<double>(x) * x;
        EXPECT_NEAR(norm, 1.0, 1e-5) << "centroids are unit norm";
    }
}

TEST(SpeakerClustering, OnlyLongSlicesVoteOnTheCount) {
    std::vector<std::vector<float>> embeddings;
    std::vector<std::uint64_t> durations;
    AddVoices(3, kLong, 0, embeddings, durations);
    EXPECT_EQ(ClusterSpeakers(embeddings, durations).count, 3) << "a third long voice counts";

    embeddings.clear();
    durations.clear();
    AddVoices(2, kLong, 0, embeddings, durations);
    AddVoices(1, kShort, 2, embeddings, durations);  // many short slices from a third voice
    EXPECT_EQ(ClusterSpeakers(embeddings, durations).count, 2)
        << "short slices cannot change the count";
}

TEST(SpeakerClustering, SparseInputsFallBackSafely) {
    EXPECT_TRUE(ClusterSpeakers({}, {}).labels.empty()) << "nothing";

    const auto one = ClusterSpeakers({Voice(0, 0)}, {kLong});
    EXPECT_EQ(one.labels, (std::vector<int>{0})) << "one slice";
    EXPECT_EQ(one.count, 1);

    const auto two = ClusterSpeakers({Voice(0, 0), Voice(1, 0)}, {kLong, kLong});
    EXPECT_EQ(two.count, 1) << "too few long slices collapse to one cluster";
    EXPECT_EQ(two.labels, (std::vector<int>{0, 0}));
    EXPECT_TRUE(two.centroids.empty());

    // No slice reaches the fit threshold, so the fit uses every slice
    std::vector<std::vector<float>> embeddings;
    std::vector<std::uint64_t> durations;
    AddVoices(2, kShort, 0, embeddings, durations);
    EXPECT_EQ(ClusterSpeakers(embeddings, durations).count, 2) << "all short slices";
}

}  // namespace
}  // namespace clinicavt::diar
