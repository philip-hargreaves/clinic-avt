#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/diarisation/embeddings.hpp"
#include "ports/diariser.hpp"

namespace clinicavt::diar {

// At finalise, moves a merged turn's edge chunks to another speaker when their embedding is
// nearer that centroid by the margin, working inwards until one stays. Catches the other
// speaker's sentence left at a turn edge. Chunks under kResplitMinFrames embed too poorly
inline constexpr std::uint64_t kResplitMinFrames = 9600;  // 0.6 s

inline constexpr double kResplitMargin = 0.20;  // cosine, calibrated by cross-validation

struct ResplitTurn {
    LabelledSlice slice;
    std::string text;
};

std::vector<ResplitTurn> ResplitByEmbedding(const std::vector<LabelledSlice>& turns,
                                            const std::vector<std::string>& texts,
                                            const std::vector<std::vector<asr::Turn>>& chunks,
                                            const EmbedRangeFn& embed,
                                            const std::vector<std::vector<float>>& centroids,
                                            double margin = kResplitMargin);

}  // namespace clinicavt::diar
