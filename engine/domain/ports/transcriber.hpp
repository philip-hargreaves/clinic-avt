#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace clinicavt::asr {

struct Turn {
    std::uint64_t first_frame = 0;
    std::uint64_t frame_count = 0;
    std::string speaker;
    std::string text;
};

inline std::string JoinedText(std::span<const Turn> chunks) {
    std::string text;
    for (const auto& chunk : chunks) {
        if (chunk.text.empty()) continue;
        if (!text.empty()) text += ' ';
        text += chunk.text;
    }
    return text;
}

using StopFn = std::function<bool()>;

// Transcribes one speaker turn per call
class ITranscriber {
   public:
    virtual ~ITranscriber() = default;

    // Returns the turn's text in timed chunks. A clip still waiting when `stop` is true is dropped
    virtual std::vector<Turn> DecodeClipChunks(std::span<const float> frames,
                                               std::uint64_t first_frame, const StopFn& stop) = 0;

    // Chunk boundaries since the last call, used to split turns
    virtual std::vector<std::uint64_t> TakeClipCuts() {
        return {};
    }
};

}  // namespace clinicavt::asr
