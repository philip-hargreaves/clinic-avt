#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "adapters/diarisation/segmenter.hpp"
#include "adapters/diarisation/speaker_clustering.hpp"
#include "adapters/diarisation/speaker_embedder.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "core/diarisation/embeddings.hpp"
#include "ports/diariser.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::diar {

// Max decodes per speculation pass, so a long pass cannot starve the causal
// stages that advance the settled frontier
inline constexpr int kSpeculateBudget = 4;
inline constexpr int kEdgeEmbedBudget = 2;  // edge chunks embedded per capture tick

// The slicing, embedding and labelling that capture and finalise share, so a
// capture-phase label is the one finalise gives the same span

// Speech regions ending by `frontier`, cut at the change points and at the
// clip cuts that snap onto a pause
std::vector<Region> CutSlices(std::span<const float> probabilities, std::uint64_t total_frames,
                              std::vector<std::uint64_t> change_points,
                              std::span<const std::uint64_t> clip_cuts,
                              std::uint64_t frontier = std::numeric_limits<std::uint64_t>::max());

struct EmbeddedSlices {
    std::vector<Region> kept;  // the slices long enough to embed
    std::vector<std::vector<float>> embeddings;
    std::vector<std::uint64_t> durations;
};

// embed gives a slice's embedding, empty when it is too short
EmbeddedSlices EmbedSlices(const std::vector<Region>& slices,
                           const std::function<std::vector<float>(const Region&)>& embed);

// The kept slices under their labels, plus long overlaps as second turns,
// in time order
std::vector<LabelledSlice> LabelSlices(const std::vector<Region>& kept,
                                       const ClusterResult& clusters,
                                       const std::vector<Region>& overlap_spans,
                                       const EmbedRangeFn& embed_span);

struct CaptureDiarisation {
    std::vector<float> vad_probabilities;  // one per hop
    SegResult seg;                         // absolute frames
    std::uint64_t seg_done = 0;            // frames fully segmented
    // Slice span -> embedding. Empty means too short to embed
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<float>> embeddings;
    TurnTexts turn_texts;    // the speculation cache, keyed on exact decode spans
    TurnChunks turn_chunks;  // the chunks behind it, same keys
    // Edge-chunk span -> embedding, computed during capture for the re-split
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<float>> chunk_embeddings;
    std::vector<std::uint64_t> clip_cuts;  // segment edges from clip decodes
};

// Provisional opening of the sealed transcript from the last Advance
struct Speculation {
    std::vector<LabelledSlice> turns;  // settled, merged, text known
    std::vector<std::string> texts;
    std::vector<std::vector<float>> centroids;  // provisional clusters
    int cluster_count = 0;
};

// Capture-phase diarisation up to the settled frontier, so finalise only processes the tail. The
// results are bit-identical to a batch run
class CaptureStage {
   public:
    CaptureStage(audio::SileroVad& vad, Segmenter& segmenter, SpeakerEmbedder& embedder);

    // Takes the audio so far. decode re-transcribes a clip, at most budget spans
    // per call so a stop never waits long behind speculation. stop ends the pass early
    void Advance(std::span<const float> audio, const DecodeClipFn& decode,
                 int budget = kSpeculateBudget, const StopFn& stop = {});

    // Called at the end of the audio. Pads the final hop and segments the tail
    void Finish(std::span<const float> audio);

    bool Engaged() const {
        return !state_.vad_probabilities.empty();
    }

    // Also resets VAD and caches for the next session
    CaptureDiarisation Take() {
        vad_.Reset();
        overlap_cache_.clear();
        speculation_ = {};
        return std::exchange(state_, {});
    }

    const Speculation& LastSpeculation() const {
        return speculation_;
    }

    void AddCutPoints(std::span<const std::uint64_t> cuts) {
        state_.clip_cuts.insert(state_.clip_cuts.end(), cuts.begin(), cuts.end());
    }

   private:
    const std::vector<float>& EmbedSlice(std::span<const float> audio, const Region& slice);

    // Cached so ticks do not re-embed the same overlap turns
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<float>> overlap_cache_;

    audio::SileroVad& vad_;
    Segmenter& segmenter_;
    SpeakerEmbedder& embedder_;
    CaptureDiarisation state_;
    Speculation speculation_;
};

}  // namespace clinicavt::diar
