#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/diarisation/diar_regions.hpp"
#include "ports/diariser.hpp"

namespace clinicavt::diar {

// 0.30 s: at 0.40 a clinical "No." was dropped and the note fabricated the denial
inline constexpr std::uint64_t kPerTurnMinClipFrames = 4800;

inline constexpr std::size_t kPerTurnMaxRepeat = 4;  // 5-gram degeneracy guard

// Finalise and speculation must merge identically or cache keys stop matching
std::vector<LabelledSlice> MergeByCluster(const std::vector<LabelledSlice>& slices);

// Also used as cache keys. Starts are clamped to the previous end, so a nested overlap turn
// becomes empty
std::vector<Region> DecodeSpans(const std::vector<LabelledSlice>& turns,
                                std::uint64_t audio_frames);

// Reuses cached chunks when a span's edges match a cached decode's chunk edges (e.g. a cut
// re-sliced a decoded turn)
inline constexpr std::uint64_t kAssembleTolFrames = 5600;  // 0.35 s: snap window plus span clamp

std::optional<std::vector<asr::Turn>> AssembleFromChunks(const TurnChunks& cache, std::uint64_t a,
                                                         std::uint64_t b);

// Decodes each merged turn's audio. An empty text means the turn is dropped. Cached texts are used
// only on an exact span match, so any hit rate is safe
std::vector<std::string> DecodeTurnTexts(const std::vector<LabelledSlice>& turns,
                                         std::span<const float> audio, const DecodeClipFn& decode,
                                         const TurnTexts* cache = nullptr,
                                         const TurnChunks* chunk_cache = nullptr,
                                         std::vector<std::vector<asr::Turn>>* chunks_out = nullptr);

// Returns the leading merged turns already cached, up to the first span finalise would decode.
// Spans below kPerTurnMinClipFrames are skipped, as in finalise
std::vector<LabelledSlice> SpeculatedTurns(const std::vector<LabelledSlice>& merged,
                                           std::uint64_t audio_frames, const TurnTexts& cache,
                                           std::vector<std::string>* texts);

// Most-repeated 5-gram, sliding. Legitimate speech peaks at 2
std::size_t MaxRepeatedNgram(const std::string& text);

}  // namespace clinicavt::diar
