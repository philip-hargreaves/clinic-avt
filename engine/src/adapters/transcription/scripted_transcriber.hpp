#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ports/transcriber.hpp"

namespace clinicavt::asr {

// CI stand-in when no ASR model is staged. One chunk per clip, numbered in decode order
class ScriptedTranscriber : public ITranscriber {
   public:
    std::vector<Turn> DecodeClipChunks(std::span<const float> frames,
                                       std::uint64_t first_frame) override {
        Turn turn;
        turn.first_frame = first_frame;
        turn.frame_count = frames.size();
        turn.text = "scripted turn " + std::to_string(decodes_++) + ", " +
                    std::to_string(frames.size()) + " frames";
        return {turn};
    }

    // Set by a test and returned once by TakeClipCuts
    std::vector<std::uint64_t> clip_cuts;

    std::vector<std::uint64_t> TakeClipCuts() override {
        return std::exchange(clip_cuts, {});
    }

   private:
    int decodes_ = 0;
};

}  // namespace clinicavt::asr
