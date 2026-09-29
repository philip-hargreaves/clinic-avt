#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "core/diarisation/diar_regions.hpp"

namespace clinicavt::diar {

// Whisper chunk edges can be a few hundred ms off. Each cut snaps to the quietest VAD hop in
// the window; it's dropped if there's no pause or it's too close to another cut
inline constexpr std::uint64_t kClipCutSnapFrames = 4800;    // 300 ms each side
inline constexpr std::uint64_t kClipCutMinGapFrames = 8000;  // 0.5 s from any other cut
inline std::vector<std::uint64_t> SnapClipCuts(std::span<const std::uint64_t> cuts,
                                               std::span<const float> vad_probabilities,
                                               std::span<const std::uint64_t> other_cuts) {
    std::vector<std::uint64_t> kept;
    for (const std::uint64_t cut : cuts) {
        const std::uint64_t lo = cut > kClipCutSnapFrames ? cut - kClipCutSnapFrames : 0;
        const std::uint64_t hi = cut + kClipCutSnapFrames;
        std::uint64_t best = cut;
        float best_p = 1.0f;
        for (std::size_t hop = static_cast<std::size_t>(lo / audio::kVadHopFrames);
             hop * audio::kVadHopFrames <= hi && hop < vad_probabilities.size(); ++hop) {
            if (vad_probabilities[hop] < best_p) {
                best_p = vad_probabilities[hop];
                best = static_cast<std::uint64_t>(hop) * audio::kVadHopFrames;
            }
        }
        if (best_p >= kEnter) continue;  // no pause near: mid-sentence, drop
        const auto near = [best](std::uint64_t other) {
            return (other > best ? other - best : best - other) < kClipCutMinGapFrames;
        };
        if (std::any_of(other_cuts.begin(), other_cuts.end(), near)) continue;
        if (std::any_of(kept.begin(), kept.end(), near)) continue;
        kept.push_back(best);
    }
    std::sort(kept.begin(), kept.end());
    return kept;
}

}  // namespace clinicavt::diar
