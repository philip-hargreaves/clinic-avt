#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "core/diarisation/diar_regions.hpp"
#include "ports/diariser.hpp"

namespace clinicavt::diar {

// One fbank frame (25 ms). Anything shorter can't be embedded
inline constexpr std::size_t kEmbedMinFrames = 400;

// Embeddings are unit norm, so the dot product is the cosine similarity
inline double Dot(std::span<const float> a, std::span<const float> b) {
    double dot = 0.0;
    const std::size_t dims = std::min(a.size(), b.size());
    for (std::size_t d = 0; d < dims; ++d) dot += static_cast<double>(a[d]) * b[d];
    return dot;
}

// Scales to unit length in place, the norm summed in double
inline void Normalise(std::vector<float>& v) {
    double norm = 0.0;
    for (const float x : v) norm += static_cast<double>(x) * x;
    norm = std::sqrt(norm) + 1e-9;
    for (float& x : v) x = static_cast<float>(x / norm);
}

inline std::vector<float> Gather(std::span<const float> audio, const std::vector<Region>& ranges) {
    std::vector<float> clip;
    for (const Region& range : ranges) {
        const auto first = static_cast<std::size_t>(range.first_frame);
        const auto end =
            std::min<std::size_t>(static_cast<std::size_t>(range.end_frame), audio.size());
        if (end > first) clip.insert(clip.end(), audio.begin() + first, audio.begin() + end);
    }
    return clip;
}

// Index of the nearest centroid other than `exclude`, or -1
inline int NearestOther(std::span<const float> embedding,
                        const std::vector<std::vector<float>>& centroids, int exclude) {
    int best = -1;
    double best_dot = -1e18;
    for (std::size_t c = 0; c < centroids.size(); ++c) {
        if (static_cast<int>(c) == exclude) continue;
        const double dot = Dot(embedding, centroids[c]);
        if (dot > best_dot) {
            best_dot = dot;
            best = static_cast<int>(c);
        }
    }
    return best;
}

using EmbedRangeFn = std::function<std::vector<float>(std::uint64_t first, std::uint64_t end)>;

inline constexpr std::uint64_t kOverlapTurnMinFrames = 6400;  // 0.4 s

// Overlap spans of kOverlapTurnMinFrames+ in a slice become extra turns on the nearest other
// centroid. There are none with fewer than two clusters
inline std::vector<LabelledSlice> OverlapTurns(const std::vector<Region>& slices,
                                               const std::vector<int>& labels,
                                               const std::vector<std::vector<float>>& centroids,
                                               const std::vector<Region>& overlap_spans,
                                               const EmbedRangeFn& embed) {
    std::vector<LabelledSlice> out;
    if (centroids.size() < 2) return out;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        for (const Region& span : overlap_spans) {
            const auto first = std::max(span.first_frame, slices[i].first_frame);
            const auto end = std::min(span.end_frame, slices[i].end_frame);
            if (end <= first || end - first < kOverlapTurnMinFrames) continue;
            const int second = NearestOther(embed(first, end), centroids, labels[i]);
            if (second >= 0) out.push_back({first, end, second});
        }
    }
    return out;
}

}  // namespace clinicavt::diar
