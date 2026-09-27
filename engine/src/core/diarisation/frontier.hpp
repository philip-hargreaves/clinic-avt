#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

#include "core/diarisation/diar_regions.hpp"

namespace clinicavt::diar {

// The frame below which diarisation state is final, from segmentation and
// VAD alone. A region's final
// extent depends on at most kMinSilenceFrames (close) + 2*kPadFrames (padding)
// of audio past its raw end, so behind this margin spans cannot change
inline constexpr std::uint64_t kSegFrontierMarginFrames = kMinSilenceFrames + 2 * kPadFrames;

inline std::uint64_t SegSettledFrontier(std::uint64_t seg_done, std::uint64_t vad_frames) {
    const std::uint64_t vad_safe =
        vad_frames > kSegFrontierMarginFrames ? vad_frames - kSegFrontierMarginFrames : 0;
    return std::min(seg_done, vad_safe);
}

// A turn is closed, and decoded during capture, once the VAD has been silent
// this long after its end. A stop right after the last words then has nothing
// left to decode
inline constexpr std::uint64_t kTurnCloseFrames = 9600;  // 0.6 s

inline bool TurnClosed(std::span<const float> vad_probabilities, std::uint64_t end_frame,
                       std::uint64_t close_frames = kTurnCloseFrames) {
    const auto first = static_cast<std::size_t>(end_frame / audio::kVadHopFrames);
    const auto last = static_cast<std::size_t>((end_frame + close_frames) / audio::kVadHopFrames);
    if (last >= vad_probabilities.size()) return false;  // not enough audio yet
    for (std::size_t hop = first; hop <= last; ++hop) {
        if (vad_probabilities[hop] >= kEnter) return false;
    }
    return true;
}

}  // namespace clinicavt::diar
