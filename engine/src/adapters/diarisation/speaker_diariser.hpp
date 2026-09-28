#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/diarisation/capture_stage.hpp"
#include "adapters/diarisation/segmenter.hpp"
#include "adapters/diarisation/speaker_embedder.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "core/diarisation/embeddings.hpp"
#include "ports/diariser.hpp"

namespace clinicavt::diar {

// The batch chain: VAD -> segmentation -> slices -> embeddings -> clusters,
// with long overlap spans emitted as second turns. Owns its own VAD
// (Silero is stateful) and the clinician anchor
class SpeakerDiariser : public IDiariser {
   public:
    SpeakerDiariser(const models::ModelStore& store, models::OvRuntime& runtime,
                    AnchorStore& anchors);

    DiariseResult Diarise(std::span<const float> audio) override;

    std::vector<double> AnchorSimilarities(std::span<const float> audio,
                                           const std::vector<LabelledSlice>& slices,
                                           int cluster_count) override;

    // Capture-phase work. Diarise then finalises from the accumulated state
    void Advance(std::span<const float> audio, const DecodeClipFn& decode) override {
        worker_.Advance(audio, decode);
    }

    void Settle(std::span<const float> audio, const DecodeClipFn& decode,
                const StopFn& stop) override {
        worker_.Advance(audio, decode, std::numeric_limits<int>::max(), stop);
    }

    // Capture's pass over growing tenths of the recording, decoding nothing. Its results are
    // keyed on spans, so the Settle after it matches one run alone
    void FindSpeech(std::span<const float> audio, const std::function<void(double)>& progress,
                    const StopFn& stop) override {
        const DecodeClipFn none = [](std::span<const float>, std::uint64_t) {
            return std::vector<asr::Turn>{};
        };
        constexpr std::size_t kSteps = 10;
        for (std::size_t step = 1; step <= kSteps; ++step) {
            if (stop && stop()) return;
            worker_.Advance(audio.first(audio.size() * step / kSteps), none, 0, stop);
            if (progress) progress(static_cast<double>(step) / kSteps);
        }
    }

    TurnTexts TakeTurnTexts() override {
        return std::exchange(texts_, {});
    }

    TurnChunks TakeTurnChunks() override {
        return std::exchange(chunks_, {});
    }

    std::vector<std::vector<float>> ClusterCentroids() override {
        return centroids_;
    }

    std::vector<float> EmbedVoice(std::span<const float> audio) override;

    // Reuses the voiceprint AnchorSimilarities computed for the cluster.
    // Embeds only when there is none
    std::vector<float> DoctorVoiceprint(std::span<const float> audio,
                                        const std::vector<LabelledSlice>& slices,
                                        int doctor_cluster) override;

    void AccrueVoiceprint(std::span<const float> voiceprint) override {
        if (!voiceprint.empty()) anchors_.Accrue(voiceprint);
    }

    void ReplaceAnchor(std::span<const float> voiceprint, std::uint64_t enrolled_at) override {
        anchors_.Replace(voiceprint, enrolled_at);
    }

    std::vector<float> EmbedSpan(std::span<const float> audio, std::uint64_t first,
                                 std::uint64_t end) override {
        const auto it = chunk_embeddings_.find({first, end});
        if (it != chunk_embeddings_.end()) return it->second;
        if (end <= first || end - first < kEmbedMinFrames || end > audio.size()) return {};
        return embedder_.Embed(audio.subspan(first, end - first));
    }

    // Roles named as finalise names them, with cluster centroids standing in
    // for the voiceprints against the anchor
    std::vector<asr::Turn> SpeculativeTranscript() override;

    void AddCutPoints(std::span<const std::uint64_t> cuts) override {
        worker_.AddCutPoints(cuts);
    }

    void DiscardCapture() override {
        (void)worker_.Take();
        texts_.clear();
        chunks_.clear();
        chunk_embeddings_.clear();
        voiceprints_.clear();
    }

    SpeakerEmbedder& Embedder() {
        return embedder_;
    }

   private:
    audio::SileroVad vad_;
    Segmenter segmenter_;
    SpeakerEmbedder embedder_;
    AnchorStore& anchors_;
    CaptureStage worker_;
    TurnTexts texts_;
    TurnChunks chunks_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<float>> chunk_embeddings_;
    std::vector<std::vector<float>> centroids_;    // finalise clusters, from Diarise
    std::vector<std::vector<float>> voiceprints_;  // per cluster, from AnchorSimilarities
};

}  // namespace clinicavt::diar
