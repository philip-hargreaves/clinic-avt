#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ports/audio_source.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::diar {

// Tidies the final transcript without moving words between speakers, in order:
//   1. merge one speaker's consecutive turns under kTidyMergeGapFrames apart
//   2. drop turns with no content (punctuation, disfluencies, <= 2 function words; yes/no/ok kept)
//   3. capitalise the first letter and standalone "i"; add terminal punctuation
inline constexpr std::uint64_t kTidyMergeGapFrames = audio::kSampleRate;  // 1 s

std::vector<asr::Turn> TidyTranscript(std::vector<asr::Turn> turns);

// No lexical content: only disfluencies, or at most two function words
bool NoContent(const std::string& text);

}  // namespace clinicavt::diar
