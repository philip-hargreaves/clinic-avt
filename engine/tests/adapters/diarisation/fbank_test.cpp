#include "adapters/diarisation/fbank.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace clinicavt::diar {
namespace {

std::vector<float> Tone(std::size_t samples) {
    std::vector<float> audio(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        audio[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 16000.0f);
    }
    return audio;
}

// The parity test against torch skips on CI, so this is the only CI guard on the front end
TEST(Fbank, FramesFollowSnipEdgesAndEveryBinIsMeanNormalised) {
    const auto short_input = EmbedderFbank(Tone(399));
    EXPECT_EQ(short_input.frames, 0u) << "under one frame yields nothing";
    EXPECT_TRUE(short_input.values.empty());

    // snip_edges: frames = 1 + (samples - 400) / 160
    const auto features = EmbedderFbank(Tone(16000));
    EXPECT_EQ(features.frames, 1u + (16000u - 400u) / 160u);
    ASSERT_EQ(features.values.size(), features.frames * kMelBins);

    // The embedder was trained on per-slice mean-subtracted features
    for (std::size_t bin = 0; bin < kMelBins; ++bin) {
        double mean = 0.0;
        for (std::size_t i = 0; i < features.frames; ++i) {
            mean += features.values[i * kMelBins + bin];
        }
        mean /= static_cast<double>(features.frames);
        EXPECT_NEAR(mean, 0.0, 1e-4) << "bin " << bin;
    }
}

}  // namespace
}  // namespace clinicavt::diar
