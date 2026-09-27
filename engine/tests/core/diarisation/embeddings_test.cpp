#include "core/diarisation/embeddings.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "core/diarisation/diar_regions.hpp"

namespace clinicavt::diar {
namespace {

TEST(Embeddings, GatherClampsToTheRecording) {
    const std::vector<float> audio{1, 2, 3, 4, 5};
    EXPECT_EQ(Gather(audio, {{1, 3}, {4, 9}}), (std::vector<float>{2, 3, 5}));
    EXPECT_TRUE(Gather(audio, {{7, 9}}).empty());
}

TEST(Embeddings, OverlapTurnsNeedTwoClustersAndALongEnoughSpan) {
    const std::vector<Region> slices{{0, 16000}, {16000, 32000}};
    const std::vector<int> labels{0, 1};
    const std::vector<std::vector<float>> centroids{{1.0f, 0.0f}, {0.0f, 1.0f}};
    int embeds = 0;
    const EmbedRangeFn embed = [&](std::uint64_t, std::uint64_t) {
        ++embeds;
        return std::vector<float>{0.0f, 1.0f};
    };
    // Shorter than kOverlapTurnMinFrames inside the first slice, long enough inside the second
    const std::vector<Region> spans{{1000, 2000}, {20000, 30000}};

    const auto turns = OverlapTurns(slices, labels, centroids, spans, embed);

    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].first_frame, 20000u);
    EXPECT_EQ(turns[0].end_frame, 30000u);
    EXPECT_EQ(turns[0].cluster, 0) << "the second turn goes to the other speaker";
    EXPECT_EQ(embeds, 1);
    EXPECT_TRUE(OverlapTurns(slices, labels, {}, spans, embed).empty()) << "one cluster: nothing";
}

struct CountingVad : audio::IStreamingVad {
    int hops = 0;
    float SpeechProbability(std::span<const float> hop) override {
        ++hops;
        return hop.size() == audio::kVadHopFrames ? 1.0f : 0.0f;
    }
    void Reset() override {}
};

TEST(Embeddings, AppendVadHopsPadsOnlyWhenAsked) {
    CountingVad vad;
    const std::vector<float> audio(audio::kVadHopFrames * 2 + 100, 0.5f);
    std::vector<float> probabilities;

    AppendVadHops(vad, audio, probabilities, false);
    EXPECT_EQ(probabilities.size(), 2u) << "whole hops only";
    AppendVadHops(vad, audio, probabilities, false);
    EXPECT_EQ(probabilities.size(), 2u) << "nothing new to read";
    AppendVadHops(vad, audio, probabilities, true);
    EXPECT_EQ(probabilities.size(), 3u) << "the tail is padded to one hop";
    EXPECT_EQ(vad.hops, 3);
    EXPECT_EQ(probabilities.back(), 1.0f) << "the padded hop is a full hop";
}

}  // namespace
}  // namespace clinicavt::diar
