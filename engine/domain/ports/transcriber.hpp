#pragma once

#include <cstdint>
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

// Transcribes one clip per call. The diariser passes each turn's audio and gets the chunks back
class ITranscriber {
   public:
    virtual ~ITranscriber() = default;

    // Whisper chunks for the clip, in absolute frames. Safe mid-session. Empty when unsupported,
    // and a re-split then keeps the original turn
    virtual std::vector<Turn> DecodeClipChunks(std::span<const float> frames,
                                               std::uint64_t first_frame) = 0;

    // Chunk edges (absolute frames, inside the clip) from every decode since the last
    // call, used as diariser cut points. Empty when unsupported
    virtual std::vector<std::uint64_t> TakeClipCuts() {
        return {};
    }
};

}  // namespace clinicavt::asr
