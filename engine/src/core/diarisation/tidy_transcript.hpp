#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ports/audio_source.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::diar {

// The finished transcript tidied as a professional transcriber would. Three
// rules in this order, and none moves a word between speakers:
//   1. one speaker's consecutive turns under kTidyMergeGapFrames apart merge, so a
//      fragment cut from its sentence ("So I..." / "to know what's going on.") rejoins it
//   2. a turn with no lexical content (punctuation alone, disfluencies, or one or two
//      stranded function words) is dropped. Yes, no, ok and any content word never are
//   3. every turn starts with a capital and ends with terminal punctuation,
//      and a standalone "i" is "I"
inline constexpr std::uint64_t kTidyMergeGapFrames = audio::kSampleRate;  // 1 s

std::vector<asr::Turn> TidyTranscript(std::vector<asr::Turn> turns);

// No lexical content: only disfluencies, or at most two function words
bool NoContent(const std::string& text);

}  // namespace clinicavt::diar
