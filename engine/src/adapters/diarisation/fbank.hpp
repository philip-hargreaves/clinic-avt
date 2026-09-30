#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace clinicavt::diar {

inline constexpr std::size_t kMelBins = 80;

struct FbankFeatures {
    std::vector<float> values;  // frames x kMelBins, row-major
    std::size_t frames = 0;
};

// Kaldi fbank matching the research setup. Input must be float32 [-1, 1] mono 16 kHz, since
// int16-range values break parity
FbankFeatures EmbedderFbank(std::span<const float> audio);

}  // namespace clinicavt::diar
