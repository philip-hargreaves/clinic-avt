#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/diarisation/cluster_voiceprint.hpp"
#include "adapters/diarisation/speaker_diariser.hpp"
#include "dev_wav.hpp"
#include "support/primock.hpp"

namespace clinicavt::diar {
namespace {

// Shape checks only. Output quality is not asserted here

void ExpectSameDiarisation(const DiariseResult& got, const DiariseResult& want) {
    ASSERT_EQ(got.slices.size(), want.slices.size());
    for (std::size_t i = 0; i < got.slices.size(); ++i) {
        EXPECT_EQ(got.slices[i].first_frame, want.slices[i].first_frame);
        EXPECT_EQ(got.slices[i].end_frame, want.slices[i].end_frame);
        EXPECT_EQ(got.slices[i].cluster, want.slices[i].cluster);
    }
    EXPECT_EQ(got.cluster_count, want.cluster_count);
}

// Voiceprints are computed off the Diarise path, overlapping the GPU turn decode, and reused
// by DoctorVoiceprint. Values must match the reference method and the reuse must skip the embed
TEST(DiariserPipeline, AConsultDiarisesToTwoSpeakersAndTheTaughtAnchorRanksItsCluster) {
    const std::string wav = test::PrimockPath(test::kPrimockMixed);
    if (wav.empty()) {
        GTEST_SKIP() << test::PrimockSkipReason(test::kPrimockMixed);
    }
    const auto audio = LoadDevWav(wav);
    const models::ModelStore store{std::filesystem::path(CLINICAVT_MODELS_DIR)};
    models::OvRuntime runtime;
    const auto anchor_root = std::filesystem::temp_directory_path() / "clinicavt-diar-anchor-test";
    std::filesystem::remove_all(anchor_root);
    std::filesystem::create_directories(anchor_root);
    AnchorStore diariser_anchors(anchor_root);
    SpeakerDiariser diariser(store, runtime, diariser_anchors);

    // Session one: shape checks, and a fresh root has no anchor to rank against
    const auto start = std::chrono::steady_clock::now();
    const auto first = diariser.Diarise(audio);
    const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    const auto& slices = first.slices;
    ASSERT_GT(slices.size(), 20u) << "a full consult yields a real turn structure";
    std::set<int> clusters;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        clusters.insert(slices[i].cluster);
        EXPECT_LT(slices[i].first_frame, slices[i].end_frame);
        EXPECT_LE(slices[i].end_frame, audio.size() + kSegWindowFrames);
        if (i > 0) EXPECT_GE(slices[i].first_frame, slices[i - 1].first_frame) << "time-sorted";
    }
    EXPECT_EQ(clusters.size(), 2u) << "doctor and patient, no phantom third voice";
    ASSERT_EQ(first.cluster_count, 2);
    EXPECT_TRUE(diariser.AnchorSimilarities(audio, slices, first.cluster_count).empty())
        << "no anchor has accrued in a fresh root";
    std::printf("diarised %zu slices, %zu clusters in %.1f s\n", slices.size(), clusters.size(),
                took.count());

    diariser.AccrueVoiceprint(diariser.DoctorVoiceprint(audio, first.slices, 0));

    // Session two: similarities equal the reference voiceprint against the anchor
    const auto second = diariser.Diarise(audio);
    const auto similarity = diariser.AnchorSimilarities(audio, second.slices, second.cluster_count);
    ASSERT_EQ(similarity.size(), 2u);
    AnchorStore anchors(anchor_root);
    const auto anchor = anchors.Anchor();
    ASSERT_TRUE(anchor.has_value());
    for (int c = 0; c < 2; ++c) {
        const auto voiceprint = ClusterVoiceprint(diariser.Embedder(), audio, second.slices, c);
        ASSERT_FALSE(voiceprint.empty());
        double dot = 0.0;
        for (std::size_t d = 0; d < voiceprint.size(); ++d) {
            dot += static_cast<double>(voiceprint[d]) * (*anchor)[d];
        }
        EXPECT_DOUBLE_EQ(similarity[static_cast<std::size_t>(c)], dot) << "cluster " << c;
    }
    EXPECT_GT(similarity[0], similarity[1]) << "the taught cluster ranks nearer";

    // Accrue reuses the voiceprint Diarise computed, so the cluster is not embedded again
    const auto accrue_start = std::chrono::steady_clock::now();
    diariser.AccrueVoiceprint(diariser.DoctorVoiceprint(audio, second.slices, 0));
    const auto accrue_took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - accrue_start);
    EXPECT_LT(accrue_took.count(), 0.05) << "a re-embed of the cluster takes hundreds of ms";
    std::error_code ec;
    std::filesystem::remove_all(anchor_root, ec);
}

TEST(DiariserPipeline, CaptureFedDiariseMatchesBatchExactly) {
    const std::string wav = test::PrimockPath(test::kPrimockMixed);
    if (wav.empty()) {
        GTEST_SKIP() << test::PrimockSkipReason(test::kPrimockMixed);
    }
    const auto audio = LoadDevWav(wav);
    const models::ModelStore store{std::filesystem::path(CLINICAVT_MODELS_DIR)};
    models::OvRuntime runtime;
    const auto root = std::filesystem::temp_directory_path() / "clinicavt-diar-capture-test";
    std::filesystem::create_directories(root);
    AnchorStore batch_anchors(root / "a");
    SpeakerDiariser batch(store, runtime, batch_anchors);
    AnchorStore fed_anchors(root / "b");
    SpeakerDiariser fed(store, runtime, fed_anchors);

    // Synthetic reconciled turns, 6 s each. Edges double as slice cuts

    const DecodeClipFn decode = [](std::span<const float> clip, std::uint64_t first) {
        asr::Turn chunk;
        chunk.first_frame = first;
        chunk.frame_count = clip.size();
        chunk.text = "re-decoded";
        return std::vector<asr::Turn>{chunk};
    };
    for (std::uint64_t fed_to = 80000; fed_to < audio.size(); fed_to += 80000) {  // 5 s steps
        fed.Advance(std::span(audio).first(fed_to), decode);
    }

    const auto want = batch.Diarise(audio);
    const auto got = fed.Diarise(audio);

    ExpectSameDiarisation(got, want);
    EXPECT_FALSE(fed.TakeTurnTexts().empty()) << "capture speculated turn texts";
    EXPECT_TRUE(batch.TakeTurnTexts().empty()) << "nothing accumulates without Advance";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// Imports find speech in a separate pass to report it. The settle afterwards must match a plain
// settle
TEST(DiariserPipeline, FindingSpeechFirstChangesNothingTheSettleDoes) {
    const std::string wav = test::PrimockPath(test::kPrimockMixed);
    if (wav.empty()) {
        GTEST_SKIP() << test::PrimockSkipReason(test::kPrimockMixed);
    }
    const auto audio = LoadDevWav(wav);
    const models::ModelStore store{std::filesystem::path(CLINICAVT_MODELS_DIR)};
    models::OvRuntime runtime;
    const auto root = std::filesystem::temp_directory_path() / "clinicavt-diar-speech-test";
    std::filesystem::create_directories(root);
    AnchorStore alone_anchors(root / "a");
    SpeakerDiariser alone(store, runtime, alone_anchors);
    AnchorStore first_anchors(root / "b");
    SpeakerDiariser first(store, runtime, first_anchors);

    const DecodeClipFn decode = [](std::span<const float> clip, std::uint64_t at) {
        asr::Turn chunk;
        chunk.first_frame = at;
        chunk.frame_count = clip.size();
        chunk.text = "decoded at " + std::to_string(at);
        return std::vector<asr::Turn>{chunk};
    };
    alone.Settle(audio, decode, {});
    std::vector<double> reported;
    first.FindSpeech(audio, [&](double fraction) { reported.push_back(fraction); }, {});
    first.Settle(audio, decode, {});

    const auto want = alone.Diarise(audio);
    const auto got = first.Diarise(audio);
    ExpectSameDiarisation(got, want);
    EXPECT_EQ(first.TakeTurnTexts(), alone.TakeTurnTexts()) << "the same spans decoded";
    ASSERT_EQ(reported.size(), 10u);
    EXPECT_DOUBLE_EQ(reported.back(), 1.0);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

}  // namespace
}  // namespace clinicavt::diar
