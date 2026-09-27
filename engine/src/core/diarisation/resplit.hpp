#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/diarisation/embeddings.hpp"
#include "ports/diariser.hpp"

namespace clinicavt::diar {

// A merged turn can carry the other speaker's sentence at an
// edge when the segmenter's boundary fell short and no cut caught it. At finalise
// each turn's edge chunks are embedded and compared with the cluster centroids. A
// chunk nearer the other cluster by the margin becomes that speaker's turn. Only
// edge chunks move, inwards until one stays, so a turn loses a borrowed head or
// tail but is never shuffled. Chunks under kResplitMinFrames embed too poorly to move
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
