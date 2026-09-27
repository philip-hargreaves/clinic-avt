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

// The text of a decode: its chunks joined
inline std::string JoinedText(std::span<const Turn> chunks) {
    std::string text;
    for (const auto& chunk : chunks) {
        if (chunk.text.empty()) continue;
        if (!text.empty()) text += ' ';
        text += chunk.text;
    }
    return text;
}

// Speech to text, one clip at a time: the diariser hands over each turn's
// audio and takes the chunks back
class ITranscriber {
   public:
    virtual ~ITranscriber() = default;

    // Whisper's chunks for the clip, with absolute frames. Safe mid-session,
    // empty when unsupported (a re-split then keeps the original turn)
    virtual std::vector<Turn> DecodeClipChunks(std::span<const float> frames,
                                               std::uint64_t first_frame) = 0;

    // Chunk edges (absolute frames, inside the clip) from every decode since
    // the last call. The diariser takes them as cut points. Empty when
    // unsupported
    virtual std::vector<std::uint64_t> TakeClipCuts() {
        return {};
    }
};

}  // namespace clinicavt::asr
