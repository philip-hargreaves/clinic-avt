#include "adapters/diarisation/capture_stage.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <string>

#include "core/common/log.hpp"
#include "core/diarisation/clip_cuts.hpp"
#include "core/diarisation/diar_regions.hpp"
#include "core/diarisation/embeddings.hpp"
#include "core/diarisation/frontier.hpp"
#include "core/diarisation/resplit.hpp"
#include "core/diarisation/slice_refinement.hpp"
#include "core/diarisation/turn_decode.hpp"
#include "ports/audio_source.hpp"

namespace clinicavt::diar {

namespace {

// Shifts a segmenter run starting at `offset` into absolute frames
void AppendSeg(SegResult& seg, const SegResult& part, std::uint64_t offset) {
    for (const auto c : part.change_points) seg.change_points.push_back(c + offset);
    for (const auto& span : part.overlap_spans) {
        seg.overlap_spans.push_back({span.first_frame + offset, span.end_frame + offset});
    }
}

}  // namespace

std::vector<Region> CutSlices(std::span<const float> probabilities, std::uint64_t total_frames,
                              std::vector<std::uint64_t> change_points,
                              std::span<const std::uint64_t> clip_cuts, std::uint64_t frontier) {
    const auto cuts = SnapClipCuts(clip_cuts, probabilities, change_points);
    change_points.insert(change_points.end(), cuts.begin(), cuts.end());
    std::sort(change_points.begin(), change_points.end());
    auto regions = SpeechRegions(probabilities, total_frames);
    std::erase_if(regions, [frontier](const Region& r) { return r.end_frame > frontier; });
    return RefineRegions(regions, change_points);
}

EmbeddedSlices EmbedSlices(const std::vector<Region>& slices,
                           const std::function<std::vector<float>(const Region&)>& embed) {
    EmbeddedSlices out;
    for (const Region& slice : slices) {
        auto embedding = embed(slice);
        if (embedding.empty()) continue;
        out.embeddings.push_back(std::move(embedding));
        out.durations.push_back(slice.end_frame - slice.first_frame);
        out.kept.push_back(slice);
    }
    return out;
}

std::vector<LabelledSlice> LabelSlices(const std::vector<Region>& kept,
                                       const ClusterResult& clusters,
                                       const std::vector<Region>& overlap_spans,
                                       const EmbedRangeFn& embed_span) {
    std::vector<LabelledSlice> labelled;
    labelled.reserve(kept.size());
    for (std::size_t i = 0; i < kept.size(); ++i) {
        labelled.push_back({kept[i].first_frame, kept[i].end_frame, clusters.labels[i]});
    }
    const auto overlaps =
        OverlapTurns(kept, clusters.labels, clusters.centroids, overlap_spans, embed_span);
    labelled.insert(labelled.end(), overlaps.begin(), overlaps.end());
    std::sort(labelled.begin(), labelled.end(), [](const LabelledSlice& a, const LabelledSlice& b) {
        return a.first_frame < b.first_frame;
    });
    return labelled;
}

CaptureStage::CaptureStage(audio::SileroVad& vad, Segmenter& segmenter, SpeakerEmbedder& embedder)
    : vad_(vad), segmenter_(segmenter), embedder_(embedder) {
    vad_.Reset();
}

void CaptureStage::Finish(std::span<const float> audio) {
    auto& s = state_;
    AppendVadHops(vad_, audio, s.vad_probabilities, true);
    if (s.seg_done < audio.size()) {
        AppendSeg(s.seg, segmenter_.Run(audio.subspan(s.seg_done)), s.seg_done);
        s.seg_done = audio.size();
    }
}

const std::vector<float>& CaptureStage::EmbedSlice(std::span<const float> audio,
                                                   const Region& slice) {
    auto& slot = state_.embeddings[{slice.first_frame, slice.end_frame}];
    if (slot.empty()) {
        const auto clip = Gather(audio, EmbeddingRanges(slice, state_.seg.overlap_spans));
        if (clip.size() >= kEmbedMinFrames) slot = embedder_.Embed(clip);
    }
    return slot;
}

void CaptureStage::Advance(std::span<const float> audio, const DecodeClipFn& decode, int budget,
                           const StopFn& stop) {
    auto& s = state_;
    const auto stopped = [&stop] { return stop && stop(); };

    // Whole hops only. Finalise pads the final partial one
    AppendVadHops(vad_, audio, s.vad_probabilities, false);

    while (s.seg_done + kSegWindowFrames <= audio.size()) {
        if (stopped()) return;
        AppendSeg(s.seg, segmenter_.Run(audio.subspan(s.seg_done, kSegWindowFrames)), s.seg_done);
        s.seg_done += kSegWindowFrames;
    }

    const auto settled =
        SegSettledFrontier(s.seg_done, s.vad_probabilities.size() * audio::kVadHopFrames);
    if (settled == 0) return;
    // No budget means finalise catch-up; only that pass logs phase timings
    const bool catch_up = budget == std::numeric_limits<int>::max();
    using Clock = std::chrono::steady_clock;
    const auto t_start = Clock::now();
    const auto seconds = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    };

    // Same slicing as finalise, up to the settled frontier
    const auto slices =
        CutSlices(s.vad_probabilities, s.vad_probabilities.size() * audio::kVadHopFrames,
                  s.seg.change_points, s.clip_cuts, settled);
    const auto t_embed = Clock::now();
    const auto embedded = EmbedSlices(slices, [&](const Region& slice) -> std::vector<float> {
        if (stopped()) return {};
        return EmbedSlice(audio, slice);
    });
    const auto& kept = embedded.kept;
    if (kept.size() < 2 || stopped()) return;

    // Provisional labels; spans that final clustering changes are never read
    const auto t_cluster = Clock::now();
    const auto clusters = ClusterSpeakers(embedded.embeddings, embedded.durations);
    // Overlap embeddings cached separately from slice embeddings (same span,
    // raw audio); this runs every tick over all settled overlaps
    const auto labelled = LabelSlices(
        kept, clusters, s.seg.overlap_spans, [&](std::uint64_t first, std::uint64_t end) {
            auto& embedding = overlap_cache_[{first, end}];
            if (embedding.empty()) {
                embedding = embedder_.Embed(Gather(audio, {{first, end}}));
            }
            return embedding;
        });

    // Decode settled turns into the cache. The last turn may still grow unless
    // closed. The catch-up decodes everything
    auto merged = MergeByCluster(labelled);
    if (merged.size() < 2) return;
    // A last turn closes on silence after it, or once the frontier is
    // kTurnCloseFrames past its end (the next speaker may have started at once)
    if (!catch_up) {
        const auto end = merged.back().end_frame;
        const bool frontier_past = settled > end && settled - end >= kTurnCloseFrames;
        if (!frontier_past && !TurnClosed(s.vad_probabilities, end)) merged.pop_back();
    }
    const auto spans = DecodeSpans(merged, audio.size());
    const auto t_decode = Clock::now();
    int decoded = 0;
    int cached = 0;
    double decoded_audio = 0.0;
    {
        for (std::size_t i = 0; i < spans.size(); ++i) {
            const auto a = spans[i].first_frame;
            const auto b = spans[i].end_frame;
            if (a >= b || b - a < kPerTurnMinClipFrames) continue;
            if (s.turn_texts.contains({a, b})) {
                ++cached;
                continue;
            }
            if (auto assembled = AssembleFromChunks(s.turn_chunks, a, b)) {
                s.turn_texts[{a, b}] = asr::JoinedText(*assembled);
                s.turn_chunks[{a, b}] = std::move(*assembled);
                ++cached;
                continue;
            }
            if (budget-- <= 0) break;
            if (stopped()) return;
            auto chunks_of_span = decode(audio.subspan(a, b - a), a);
            const std::string text = asr::JoinedText(chunks_of_span);
            ++decoded;
            decoded_audio += static_cast<double>(b - a) / audio::kSampleRate;
            if (catch_up) {
                log::Printf("clinicavt-engine: catch-up decoded %.1f-%.1f s\n",
                            static_cast<double>(a) / audio::kSampleRate,
                            static_cast<double>(b) / audio::kSampleRate);
            }
            if (text.empty()) continue;  // rejected, silent, or stopping
            if (MaxRepeatedNgram(text) >= kPerTurnMaxRepeat) continue;
            s.turn_texts[{a, b}] = text;
            s.turn_chunks[{a, b}] = std::move(chunks_of_span);
        }
    }
    if (catch_up) {
        const auto t_end = Clock::now();
        log::Printf(
            "clinicavt-engine: catch-up frontier %.1f of %.1f s, slices %zu, "
            "segment+cuts %.2f s, embed %.2f s, cluster %.2f s, decode %d spans "
            "(%.1f s audio, %d cached) %.2f s, total %.2f s\n",
            static_cast<double>(settled) / audio::kSampleRate,
            static_cast<double>(audio.size()) / audio::kSampleRate, kept.size(),
            seconds(t_start, t_embed), seconds(t_embed, t_cluster), seconds(t_cluster, t_decode),
            decoded, decoded_audio, cached, seconds(t_decode, t_end), seconds(t_start, t_end));
    }

    // Edge chunks are embedded now, a few per tick, so the finalise re-split
    // only takes dot products
    {
        int embeds = catch_up ? std::numeric_limits<int>::max() : kEdgeEmbedBudget;
        for (std::size_t i = 0; i < merged.size() && embeds > 0; ++i) {
            const auto key = std::make_pair(spans[i].first_frame, spans[i].end_frame);
            const auto it = s.turn_chunks.find(key);
            if (it == s.turn_chunks.end() || it->second.size() < 2) continue;
            const auto& parts = it->second;
            const std::size_t idx[4] = {0, 1, parts.size() - 2, parts.size() - 1};
            for (const std::size_t j : idx) {
                if (embeds <= 0) break;
                const auto& p = parts[j];
                const std::uint64_t lo = std::max(p.first_frame, merged[i].first_frame);
                const std::uint64_t hi =
                    std::min(p.first_frame + p.frame_count, merged[i].end_frame);
                if (hi <= lo || hi - lo < kResplitMinFrames) continue;
                if (s.chunk_embeddings.contains({lo, hi})) continue;
                s.chunk_embeddings[{lo, hi}] = embedder_.Embed(audio.subspan(lo, hi - lo));
                --embeds;
            }
        }
    }
    speculation_.turns = SpeculatedTurns(merged, audio.size(), s.turn_texts, &speculation_.texts);
    speculation_.centroids = clusters.centroids;
    speculation_.cluster_count = clusters.count;
    // The prefill reads this transcript, so re-split exactly as finalise does or
    // the prompts diverge
    if (clusters.count >= 2 && !speculation_.turns.empty()) {
        const auto spec_spans = DecodeSpans(speculation_.turns, audio.size());
        std::vector<std::vector<asr::Turn>> chunks(speculation_.turns.size());
        for (std::size_t i = 0; i < spec_spans.size(); ++i) {
            const auto it =
                s.turn_chunks.find({spec_spans[i].first_frame, spec_spans[i].end_frame});
            if (it != s.turn_chunks.end()) chunks[i] = it->second;
        }
        const auto pieces = ResplitByEmbedding(
            speculation_.turns, speculation_.texts, chunks,
            [&](std::uint64_t first, std::uint64_t end) -> std::vector<float> {
                const auto it = s.chunk_embeddings.find({first, end});
                if (it != s.chunk_embeddings.end()) return it->second;
                return {};  // not embedded yet, retried next tick
            },
            clusters.centroids);
        speculation_.turns.clear();
        speculation_.texts.clear();
        for (const auto& piece : pieces) {
            speculation_.turns.push_back(piece.slice);
            speculation_.texts.push_back(piece.text);
        }
    }
}

}  // namespace clinicavt::diar
