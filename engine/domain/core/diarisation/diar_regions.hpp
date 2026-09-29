#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "ports/streaming_vad.hpp"

namespace clinicavt::diar {

// Silero thresholds for the whole-recording batch pass. Validated; do not retune
inline constexpr float kEnter = 0.40f;
inline constexpr float kExit = 0.25f;
inline constexpr std::uint64_t kMinSpeechFrames = 1600;   // 100 ms
inline constexpr std::uint64_t kMinSilenceFrames = 2400;  // 150 ms
inline constexpr std::uint64_t kPadFrames = 800;          // 50 ms

struct Region {
    std::uint64_t first_frame = 0;
    std::uint64_t end_frame = 0;
};

// Runs the VAD on hops of `audio` not yet in `probabilities`. A final partial hop is
// zero-padded only when pad_tail is set
inline void AppendVadHops(audio::IStreamingVad& vad, std::span<const float> audio,
                          std::vector<float>& probabilities, bool pad_tail) {
    std::vector<float> hop(audio::kVadHopFrames, 0.0f);
    for (;;) {
        const auto at = probabilities.size() * audio::kVadHopFrames;
        if (at >= audio.size()) return;
        const auto have = std::min<std::size_t>(audio::kVadHopFrames, audio.size() - at);
        if (have < audio::kVadHopFrames && !pad_tail) return;
        std::copy_n(audio.begin() + static_cast<std::ptrdiff_t>(at), have, hop.begin());
        std::fill(hop.begin() + static_cast<std::ptrdiff_t>(have), hop.end(), 0.0f);
        probabilities.push_back(vad.SpeechProbability(hop));
    }
}

// One probability per kVadHopFrames hop, as audio::IStreamingVad emits them
inline std::vector<Region> SpeechRegions(std::span<const float> probabilities,
                                         std::uint64_t total_frames) {
    std::vector<Region> regions;
    bool triggered = false;
    std::uint64_t start = 0;
    std::uint64_t temp_end = 0;
    for (std::size_t i = 0; i < probabilities.size(); ++i) {
        const std::uint64_t frame = static_cast<std::uint64_t>(i) * audio::kVadHopFrames;
        const float p = probabilities[i];
        if (p >= kEnter && temp_end != 0) temp_end = 0;
        if (p >= kEnter && !triggered) {
            triggered = true;
            start = frame;
            continue;
        }
        if (p < kExit && triggered) {
            if (temp_end == 0) temp_end = frame;
            if (frame - temp_end < kMinSilenceFrames) continue;
            if (temp_end - start > kMinSpeechFrames) regions.push_back({start, temp_end});
            temp_end = 0;
            triggered = false;
        }
    }
    if (triggered && total_frames - start > kMinSpeechFrames) {
        regions.push_back({start, total_frames});
    }

    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (i == 0) {
            regions[i].first_frame =
                regions[i].first_frame > kPadFrames ? regions[i].first_frame - kPadFrames : 0;
        }
        if (i + 1 < regions.size()) {
            const std::uint64_t gap = regions[i + 1].first_frame - regions[i].end_frame;
            if (gap < 2 * kPadFrames) {
                regions[i].end_frame += gap / 2;
                regions[i + 1].first_frame -= gap / 2;
            } else {
                regions[i].end_frame = std::min(total_frames, regions[i].end_frame + kPadFrames);
                regions[i + 1].first_frame -= kPadFrames;
            }
        } else {
            regions[i].end_frame = std::min(total_frames, regions[i].end_frame + kPadFrames);
        }
    }
    return regions;
}

}  // namespace clinicavt::diar
