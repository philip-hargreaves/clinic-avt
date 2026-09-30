#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "adapters/models/deferred_load.hpp"
#include "ports/diariser.hpp"

namespace clinicavt::diar {

// Diariser loaded on a background thread. Callers are off the audio threads
class DeferredDiariser : public IDiariser, public ICaptureDiarisation, public IVoiceprints {
   public:
    explicit DeferredDiariser(std::function<std::unique_ptr<IDiariser>()> build,
                              metrics::Registry* metrics = nullptr)
        : inner_("diarisation", std::move(build), metrics) {}

    DiariseResult Diarise(std::span<const float> audio) override {
        return inner_.Get().Diarise(audio);
    }

    std::vector<double> AnchorSimilarities(std::span<const float> audio,
                                           const std::vector<LabelledSlice>& slices,
                                           int cluster_count) override {
        return inner_.Get().AnchorSimilarities(audio, slices, cluster_count);
    }

    // Returned without waiting for the load. Each call then waits for it, and does nothing when the
    // loaded diariser lacks the capability
    ICaptureDiarisation* Capture() override {
        return this;
    }

    IVoiceprints* Voiceprints() override {
        return this;
    }

    void Advance(std::span<const float> audio, const DecodeClipFn& decode) override {
        if (auto* capture = inner_.Get().Capture()) capture->Advance(audio, decode);
    }

    void Settle(std::span<const float> audio, const DecodeClipFn& decode,
                const StopFn& stop) override {
        if (auto* capture = inner_.Get().Capture()) capture->Settle(audio, decode, stop);
    }

    void FindSpeech(std::span<const float> audio, const std::function<void(double)>& progress,
                    const StopFn& stop) override {
        if (auto* capture = inner_.Get().Capture()) capture->FindSpeech(audio, progress, stop);
    }

    TurnTexts TakeTurnTexts() override {
        auto* capture = inner_.Get().Capture();
        return capture != nullptr ? capture->TakeTurnTexts() : TurnTexts{};
    }

    TurnChunks TakeTurnChunks() override {
        auto* capture = inner_.Get().Capture();
        return capture != nullptr ? capture->TakeTurnChunks() : TurnChunks{};
    }

    std::vector<asr::Turn> SpeculativeTranscript() override {
        auto* capture = inner_.Get().Capture();
        return capture != nullptr ? capture->SpeculativeTranscript() : std::vector<asr::Turn>{};
    }

    void AddCutPoints(std::span<const std::uint64_t> cuts) override {
        if (auto* capture = inner_.Get().Capture()) capture->AddCutPoints(cuts);
    }

    void DiscardCapture() override {
        if (!inner_.Loaded()) return;
        if (auto* capture = inner_.Get().Capture()) capture->DiscardCapture();
    }

    std::vector<std::vector<float>> ClusterCentroids() override {
        auto* voiceprints = inner_.Get().Voiceprints();
        return voiceprints != nullptr ? voiceprints->ClusterCentroids()
                                      : std::vector<std::vector<float>>{};
    }

    std::vector<float> EmbedSpan(std::span<const float> audio, std::uint64_t first,
                                 std::uint64_t end) override {
        auto* voiceprints = inner_.Get().Voiceprints();
        return voiceprints != nullptr ? voiceprints->EmbedSpan(audio, first, end)
                                      : std::vector<float>{};
    }

    std::vector<float> EmbedVoice(std::span<const float> speech) override {
        auto* voiceprints = inner_.Get().Voiceprints();
        return voiceprints != nullptr ? voiceprints->EmbedVoice(speech) : std::vector<float>{};
    }

    void ReplaceAnchor(std::span<const float> voiceprint, std::uint64_t enrolled_at) override {
        if (auto* voiceprints = inner_.Get().Voiceprints()) {
            voiceprints->ReplaceAnchor(voiceprint, enrolled_at);
        }
    }

    std::vector<float> DoctorVoiceprint(std::span<const float> audio,
                                        const std::vector<LabelledSlice>& slices,
                                        int doctor_cluster) override {
        auto* voiceprints = inner_.Get().Voiceprints();
        return voiceprints != nullptr ? voiceprints->DoctorVoiceprint(audio, slices, doctor_cluster)
                                      : std::vector<float>{};
    }

    void AccrueVoiceprint(std::span<const float> voiceprint) override {
        if (auto* voiceprints = inner_.Get().Voiceprints())
            voiceprints->AccrueVoiceprint(voiceprint);
    }

   private:
    models::DeferredLoad<IDiariser> inner_;
};

}  // namespace clinicavt::diar
