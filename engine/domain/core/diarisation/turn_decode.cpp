#include "core/diarisation/turn_decode.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/common/strings.hpp"
#include "core/diarisation/diar_regions.hpp"
#include "ports/diariser.hpp"

namespace clinicavt::diar {

std::size_t MaxRepeatedNgram(const std::string& text) {
    const auto words =
        strings::LowerTokens(text, [](unsigned char c) { return std::isalnum(c) != 0; });
    if (words.size() < 6) return 0;
    std::map<std::string, std::size_t> seen;
    std::size_t worst = 0;
    for (std::size_t i = 0; i + 5 <= words.size(); ++i) {
        std::string gram;
        for (std::size_t j = i; j < i + 5; ++j) {
            gram += words[j];
            gram.push_back(' ');
        }
        worst = std::max(worst, ++seen[gram]);
    }
    return worst;
}

std::vector<LabelledSlice> MergeByCluster(const std::vector<LabelledSlice>& slices) {
    std::vector<LabelledSlice> turns;
    for (const auto& slice : slices) {
        if (!turns.empty() && turns.back().cluster == slice.cluster) {
            turns.back().end_frame = std::max(turns.back().end_frame, slice.end_frame);
        } else {
            turns.push_back(slice);
        }
    }
    return turns;
}

std::vector<Region> DecodeSpans(const std::vector<LabelledSlice>& turns,
                                std::uint64_t audio_frames) {
    std::vector<Region> spans(turns.size());
    std::uint64_t prev_end = 0;
    for (std::size_t i = 0; i < turns.size(); ++i) {
        const std::uint64_t b = std::min(turns[i].end_frame, audio_frames);
        const std::uint64_t a = std::max(turns[i].first_frame, prev_end);
        spans[i] = {a, b};
        prev_end = std::max(prev_end, b);
    }
    return spans;
}

std::optional<std::vector<asr::Turn>> AssembleFromChunks(const TurnChunks& cache, std::uint64_t a,
                                                         std::uint64_t b) {
    const auto close_to = [](std::uint64_t x, std::uint64_t y) {
        return (x > y ? x - y : y - x) <= kAssembleTolFrames;
    };
    for (const auto& [span, chunks] : cache) {
        if (span.first > a + kAssembleTolFrames || span.second + kAssembleTolFrames < b) continue;
        if (chunks.empty()) continue;
        std::vector<asr::Turn> inside;
        for (const auto& c : chunks) {
            const std::uint64_t mid = c.first_frame + c.frame_count / 2;
            if (mid >= a && mid < b) inside.push_back(c);
        }
        if (inside.empty()) continue;
        const auto& first = inside.front();
        const auto& last = inside.back();
        if (!close_to(first.first_frame, a) || !close_to(last.first_frame + last.frame_count, b))
            continue;
        return inside;
    }
    return std::nullopt;
}

std::vector<std::string> DecodeTurnTexts(const std::vector<LabelledSlice>& turns,
                                         std::span<const float> audio, const DecodeClipFn& decode,
                                         const TurnTexts* cache, const TurnChunks* chunk_cache,
                                         std::vector<std::vector<asr::Turn>>* chunks_out) {
    const auto spans = DecodeSpans(turns, audio.size());
    std::vector<std::string> texts(turns.size());
    if (chunks_out != nullptr) chunks_out->assign(turns.size(), {});
    {
        for (std::size_t i = 0; i < turns.size(); ++i) {
            const auto [a, b] = spans[i];
            if (a >= b || b - a < kPerTurnMinClipFrames) continue;
            if (cache != nullptr) {
                const auto it = cache->find({a, b});
                if (it != cache->end()) {
                    texts[i] = it->second;
                    if (chunks_out != nullptr && chunk_cache != nullptr) {
                        const auto ct = chunk_cache->find({a, b});
                        if (ct != chunk_cache->end()) (*chunks_out)[i] = ct->second;
                    }
                    continue;
                }
            }
            if (chunk_cache != nullptr) {
                if (auto assembled = AssembleFromChunks(*chunk_cache, a, b)) {
                    texts[i] = asr::JoinedText(*assembled);
                    if (chunks_out != nullptr) (*chunks_out)[i] = std::move(*assembled);
                    continue;
                }
            }
            auto chunks = decode(audio.subspan(a, b - a), a);
            std::string text = asr::JoinedText(chunks);
            // A repetition loop has no safe fallback, so leave the text empty
            if (MaxRepeatedNgram(text) >= kPerTurnMaxRepeat) continue;
            texts[i] = std::move(text);
            if (chunks_out != nullptr) (*chunks_out)[i] = std::move(chunks);
        }
    }
    return texts;
}

std::vector<LabelledSlice> SpeculatedTurns(const std::vector<LabelledSlice>& merged,
                                           std::uint64_t audio_frames, const TurnTexts& cache,
                                           std::vector<std::string>* texts) {
    const auto spans = DecodeSpans(merged, audio_frames);
    std::vector<LabelledSlice> known;
    texts->clear();
    for (std::size_t i = 0; i < merged.size(); ++i) {
        const auto [a, b] = spans[i];
        if (a >= b || b - a < kPerTurnMinClipFrames) continue;
        const auto it = cache.find({a, b});
        if (it == cache.end()) break;
        known.push_back(merged[i]);
        texts->push_back(it->second);
    }
    return known;
}

}  // namespace clinicavt::diar
