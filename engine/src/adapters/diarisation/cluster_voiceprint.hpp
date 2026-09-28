#pragma once

#include <span>
#include <vector>

#include "adapters/diarisation/speaker_embedder.hpp"
#include "core/diarisation/diar_regions.hpp"
#include "core/diarisation/embeddings.hpp"
#include "ports/audio_source.hpp"
#include "ports/diariser.hpp"

namespace clinicavt::diar {

inline constexpr std::uint64_t kVoiceprintCapFrames =
    std::uint64_t{90} * audio::kSampleRate;  // long-exposure cap
inline constexpr std::uint64_t kVoiceprintMinFrames =
    audio::kSampleRate;  // under 1 s carries no identity

// The cluster's slices in time order until the cap, the crossing slice
// kept whole. Empty under one second
inline std::vector<Region> VoiceprintRanges(const std::vector<LabelledSlice>& slices, int cluster) {
    std::vector<Region> ranges;
    std::uint64_t total = 0;
    for (const auto& slice : slices) {
        if (slice.cluster != cluster) continue;
        ranges.push_back({slice.first_frame, slice.end_frame});
        total += slice.end_frame - slice.first_frame;
        if (total >= kVoiceprintCapFrames) break;
    }
    if (total < kVoiceprintMinFrames) return {};
    return ranges;
}

// One voiceprint per cluster: its audio concatenated and embedded once,
// not a mean of per-slice embeddings. Empty when too short
inline std::vector<float> ClusterVoiceprint(SpeakerEmbedder& embedder, std::span<const float> audio,
                                            const std::vector<LabelledSlice>& slices, int cluster) {
    const auto clip = Gather(audio, VoiceprintRanges(slices, cluster));
    if (clip.size() < kVoiceprintMinFrames) return {};
    return embedder.Embed(clip);
}

}  // namespace clinicavt::diar
