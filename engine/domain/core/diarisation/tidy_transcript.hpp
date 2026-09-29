#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ports/audio_source.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::diar {

// Tidies the final transcript without moving words between speakers, in order:
//   1. merge one speaker's consecutive turns under kTidyMergeGapFrames apart
//   2. drop turns of only punctuation, disfluencies or <= 2 function words, except yes/no/ok
//   3. capitalise the first letter and standalone "i", and add terminal punctuation
inline constexpr std::uint64_t kTidyMergeGapFrames = audio::kSampleRate;  // 1 s

std::vector<asr::Turn> TidyTranscript(std::vector<asr::Turn> turns);

// True when the text is only disfluencies or at most two function words
bool NoContent(const std::string& text);

}  // namespace clinicavt::diar
