#include "adapters/diarisation/speaker_diariser.hpp"

#include <algorithm>
#include <chrono>

#include "adapters/diarisation/cluster_voiceprint.hpp"
#include "adapters/diarisation/speaker_clustering.hpp"
#include "core/diarisation/diar_regions.hpp"
#include "core/diarisation/embeddings.hpp"
#include "core/diarisation/role_naming.hpp"
#include "core/diarisation/slice_refinement.hpp"

namespace clinicavt::diar {

SpeakerDiariser::SpeakerDiariser(const models::ModelStore& store, models::OvRuntime& runtime,
                                 AnchorStore& anchors)
    : vad_(store, runtime),
      segmenter_(store, runtime),
      embedder_(store, runtime),
      anchors_(anchors),
      worker_(vad_, segmenter_, embedder_) {}

DiariseResult SpeakerDiariser::Diarise(std::span<const float> audio) {
    DiariseResult result;
    if (audio.empty()) return result;
    // Stage laps for the finalise breakdown, measurement only
    auto lap_start = std::chrono::steady_clock::now();
    const auto lap = [&lap_start] {
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - lap_start).count();
        lap_start = now;
        return seconds;
    };

    // With capture-phase state, finalise finishes from it. Without it the whole recording is
    // processed here, with the same maths
    CaptureDiarisation capture;
    std::vector<float> probabilities;
    SegResult seg;
    if (worker_.Engaged()) {
        worker_.Finish(audio);
        capture = worker_.Take();
        probabilities = std::move(capture.vad_probabilities);
        seg = std::move(capture.seg);
        texts_ = std::move(capture.turn_texts);
        chunks_ = std::move(capture.turn_chunks);
        chunk_embeddings_ = std::move(capture.chunk_embeddings);
    } else {
        vad_.Reset();
        AppendVadHops(vad_, audio, probabilities, true);
        seg = segmenter_.Run(audio);
    }

    result.timing.finish_s = lap();
    const auto slices =
        CutSlices(probabilities, audio.size(), std::move(seg.change_points), capture.clip_cuts);
    // Reuses capture embeddings where present. Empty means the slice is too short
    const auto embedded = EmbedSlices(slices, [&](const Region& slice) -> std::vector<float> {
        const auto it = capture.embeddings.find({slice.first_frame, slice.end_frame});
        if (it != capture.embeddings.end()) {
            if (!it->second.empty()) ++result.timing.embed_hits;
            return it->second;
        }
        const auto clip = Gather(audio, EmbeddingRanges(slice, seg.overlap_spans));
        if (clip.size() < kEmbedMinFrames) return {};
        ++result.timing.embed_misses;
        return embedder_.Embed(clip);
    });

    result.timing.embed_s = lap();
    const auto clusters = ClusterSpeakers(embedded.embeddings, embedded.durations);
    centroids_ = clusters.centroids;
    result.timing.cluster_s = lap();
    result.slices = LabelSlices(embedded.kept, clusters, seg.overlap_spans,
                                [&](std::uint64_t first, std::uint64_t end) {
                                    return embedder_.Embed(Gather(audio, {{first, end}}));
                                });
    result.timing.overlap_s = lap();
    result.cluster_count = clusters.count;

    voiceprints_.clear();  // AnchorSimilarities refills them per finalise
    return result;
}

std::vector<double> SpeakerDiariser::AnchorSimilarities(std::span<const float> audio,
                                                        const std::vector<LabelledSlice>& slices,
                                                        int cluster_count) {
    // Similarity of each cluster to the anchor. A cluster too short for a
    // voiceprint ranks below any real match
    const auto anchor = anchors_.Anchor();
    if (!anchor) return {};
    std::vector<double> similarity(static_cast<std::size_t>(cluster_count), -2.0);
    voiceprints_.assign(static_cast<std::size_t>(cluster_count), {});
    for (int c = 0; c < cluster_count; ++c) {
        auto voiceprint = ClusterVoiceprint(embedder_, audio, slices, c);
        if (voiceprint.empty()) continue;
        similarity[static_cast<std::size_t>(c)] = Dot(voiceprint, *anchor);
        voiceprints_[static_cast<std::size_t>(c)] = std::move(voiceprint);
    }
    return similarity;
}

std::vector<asr::Turn> SpeakerDiariser::SpeculativeTranscript() {
    const Speculation& spec = worker_.LastSpeculation();
    if (spec.turns.empty()) return {};
    std::vector<double> similarity;
    const auto anchor = anchors_.Anchor();
    if (anchor && spec.centroids.size() == static_cast<std::size_t>(spec.cluster_count)) {
        for (const auto& centroid : spec.centroids) similarity.push_back(Dot(centroid, *anchor));
    }
    return NameTurns(spec.turns, spec.texts, spec.cluster_count, similarity).turns;
}

std::vector<float> SpeakerDiariser::DoctorVoiceprint(std::span<const float> audio,
                                                     const std::vector<LabelledSlice>& slices,
                                                     int doctor_cluster) {
    const auto index = static_cast<std::size_t>(doctor_cluster);
    return index < voiceprints_.size() && !voiceprints_[index].empty()
               ? voiceprints_[index]
               : ClusterVoiceprint(embedder_, audio, slices, doctor_cluster);
}

std::vector<float> SpeakerDiariser::EmbedVoice(std::span<const float> audio) {
    if (audio.size() < kVoiceprintMinFrames) return {};
    // One embed, capped like a consultation voiceprint
    return embedder_.Embed(
        audio.subspan(0, std::min<std::size_t>(audio.size(), kVoiceprintCapFrames)));
}

}  // namespace clinicavt::diar
