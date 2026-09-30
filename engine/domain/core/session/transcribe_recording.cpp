#include "core/session/transcribe_recording.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/common/log.hpp"
#include "core/diarisation/resplit.hpp"
#include "core/diarisation/role_naming.hpp"
#include "core/diarisation/tidy_transcript.hpp"
#include "core/diarisation/turn_decode.hpp"
#include "core/metrics/metrics.hpp"
#include "ports/audio_source.hpp"
#include "ports/diariser.hpp"
#include "ports/session_events.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::session {

Transcript TranscribeRecording(std::span<const float> audio, diar::IDiariser& diariser,
                               asr::ITranscriber& transcriber, ISessionEvents& events,
                               metrics::Registry* metrics, const StageFn& stage) {
    Transcript transcript;
    events.OnProgress("speakers");
    transcript.diarised = diariser.Diarise(audio);
    const auto& result = transcript.diarised;
    stage("diarised");
    {
        const auto& t = result.timing;
        log::Printf(
            "clinicavt-engine: diarise finish %.2f s, embed %.2f s (%d hits, %d misses), "
            "cluster %.2f s, overlap %.2f s\n",
            t.finish_s, t.embed_s, t.embed_hits, t.embed_misses, t.cluster_s, t.overlap_s);
        if (metrics != nullptr) {
            metrics->RecordStage("diarise finish", t.finish_s);
            metrics->RecordStage("diarise embed", t.embed_s);
            metrics->RecordStage("diarise embed misses", t.embed_misses);
            metrics->RecordStage("diarise cluster", t.cluster_s);
            metrics->RecordStage("diarise overlap", t.overlap_s);
        }
    }
    // Longest stage on the NPU, so report it separately
    events.OnProgress("turns");
    // The future's destructor waits, so an exception cannot orphan the task
    auto voiceprint_seconds = 0.0;
    auto anchor_similarity = std::async(std::launch::async, [&] {
        const auto started = std::chrono::steady_clock::now();
        auto similarity = diariser.AnchorSimilarities(audio, result.slices, result.cluster_count);
        voiceprint_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return similarity;
    });
    const auto decode = [&transcriber](std::span<const float> clip, std::uint64_t first) {
        return transcriber.DecodeClipChunks(clip, first);
    };
    auto turns = diar::MergeByCluster(result.slices);
    auto* const capture = diariser.Capture();
    const auto cache = capture != nullptr ? capture->TakeTurnTexts() : diar::TurnTexts{};
    const auto chunk_cache = capture != nullptr ? capture->TakeTurnChunks() : diar::TurnChunks{};
    std::vector<std::vector<asr::Turn>> turn_chunks;
    auto turn_texts =
        diar::DecodeTurnTexts(turns, audio, decode, &cache, &chunk_cache, &turn_chunks);
    stage("turns decoded");
    const auto similarity = anchor_similarity.get();
    stage("voiceprints joined");
    // Must run after the voiceprint task joins because the embedder is single-threaded
    {
        auto* const voiceprints = diariser.Voiceprints();
        const auto centroids = voiceprints != nullptr ? voiceprints->ClusterCentroids()
                                                      : std::vector<std::vector<float>>{};
        const auto pieces = diar::ResplitByEmbedding(
            turns, turn_texts, turn_chunks,
            [&](std::uint64_t first, std::uint64_t end) {
                return voiceprints != nullptr ? voiceprints->EmbedSpan(audio, first, end)
                                              : std::vector<float>{};
            },
            centroids);
        turns.clear();
        turn_texts.clear();
        for (const auto& piece : pieces) {
            turns.push_back(piece.slice);
            turn_texts.push_back(piece.text);
        }
        stage("re-split");
    }
    if (metrics != nullptr) {
        metrics->RecordStage("diarise voiceprints", voiceprint_seconds);
    }
    {
        std::size_t with_text = 0;
        std::uint64_t longest = 0;
        for (std::size_t i = 0; i < turns.size(); ++i) {
            if (!turn_texts[i].empty()) ++with_text;
            longest = std::max(longest, turns[i].end_frame - turns[i].first_frame);
        }
        log::Printf(
            "clinicavt-engine: %zu turns, %zu with text, %zu cached, longest %.1f s, "
            "%d clusters\n",
            turns.size(), with_text, cache.size(),
            static_cast<double>(longest) / audio::kSampleRate, result.cluster_count);
        if (metrics != nullptr) {
            metrics->RecordTranscript(static_cast<int>(with_text), result.cluster_count);
        }
    }
    auto named = diar::NameTurns(turns, turn_texts, result.cluster_count, similarity);
    const auto& roles = named.roles;
    // Log anchor similarities and the role decision, for diagnosing wrong roles
    {
        std::string sims;
        for (const double s : similarity) {
            char one[16];
            std::snprintf(one, sizeof one, " %.3f", s);
            sims += one;
        }
        log::Printf("clinicavt-engine: roles anchor sims%s margin %.3f -> doctor %d by %s\n",
                    sims.empty() ? " none" : sims.c_str(), roles.margin, roles.doctor_cluster,
                    roles.from_anchor ? "print" : "content");
    }
    transcript.doctor_cluster = roles.doctor_cluster;
    transcript.turns = diar::TidyTranscript(std::move(named.turns));
    return transcript;
}

}  // namespace clinicavt::session
