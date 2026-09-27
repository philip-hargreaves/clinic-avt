// Dev evaluation tool. Default: production diarisation over one wav, printing
// "start end cluster" per slice in seconds (the attribution scorer's format).
// --roles runs the full finalise flow (per-turn ASR, text assignment, cold-start
// naming) and prints roles, margin and per-cluster voiceprints for the
// role-acceptance scorer
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/diarisation/cluster_voiceprint.hpp"
#include "adapters/diarisation/speaker_diariser.hpp"
#include "adapters/transcription/whisper_transcriber.hpp"
#include "core/diarisation/role_naming.hpp"
#include "core/diarisation/turn_decode.hpp"

namespace {

std::vector<float> LoadWav(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error(std::string("missing wav: ") + path);
    in.seekg(0, std::ios::end);
    const auto bytes = static_cast<std::size_t>(in.tellg()) - 44;
    in.seekg(44);
    std::vector<std::int16_t> pcm(bytes / 2);
    in.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(bytes));
    std::vector<float> frames(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) frames[i] = pcm[i] / 32768.0f;
    return frames;
}

// The clip-decode contract hands back chunks. The probe's whole-clip text is one
std::vector<clinicavt::asr::Turn> AsChunk(std::string text, std::span<const float> clip,
                                          std::uint64_t first_frame) {
    clinicavt::asr::Turn chunk;
    chunk.first_frame = first_frame;
    chunk.frame_count = clip.size();
    chunk.text = std::move(text);
    return {chunk};
}

std::string Decode(clinicavt::asr::WhisperTranscriber& transcriber, std::span<const float> clip,
                   std::uint64_t first_frame) {
    return clinicavt::asr::JoinedText(transcriber.DecodeClipChunks(clip, first_frame));
}

// --amortise-probe: feeds a SpeakerDiariser the audio so far in five-second
// steps, exactly as the session controller does, then times what a stop pays
// and verifies the output is bit-identical to the batch pass
void AmortiseProbe(const clinicavt::models::ModelStore& store,
                   clinicavt::models::OvRuntime& runtime,
                   clinicavt::asr::WhisperTranscriber& whisper, const std::vector<float>& audio,
                   const clinicavt::diar::DiariseResult& batch) {
    using Clock = std::chrono::steady_clock;
    const auto seconds = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    };
    constexpr std::uint64_t kStepFrames = 5 * 16000;

    const auto anchor_root =
        std::filesystem::temp_directory_path() / "clinicavt-diar-eval-amortise";
    std::filesystem::create_directories(anchor_root);
    clinicavt::diar::AnchorStore fed_anchors(anchor_root);
    clinicavt::diar::SpeakerDiariser fed(store, runtime, fed_anchors);
    std::size_t decodes = 0;
    const auto decode = [&whisper, &decodes](std::span<const float> clip, std::uint64_t first) {
        ++decodes;
        return AsChunk(Decode(whisper, clip, first), clip, first);
    };

    // Capture: the controller's cadence
    double capture_s = 0.0;
    std::size_t ticks = 0;
    for (std::uint64_t upto = kStepFrames; upto < audio.size(); upto += kStepFrames) {
        const auto t0 = Clock::now();
        fed.Advance(std::span<const float>(audio).first(upto), decode);
        capture_s += seconds(t0, Clock::now());
        ++ticks;
    }
    const std::size_t capture_decodes = decodes;

    // Stop: everything production's finalise pays, including the pair's
    // voiceprints for anchor ranking and accrual
    const auto stop_start = Clock::now();
    const auto result = fed.Diarise(audio);
    const auto cache = fed.TakeTurnTexts();
    const auto turns = clinicavt::diar::MergeByCluster(result.slices);
    std::size_t hits = 0;
    for (const auto& span : clinicavt::diar::DecodeSpans(turns, audio.size())) {
        if (cache.contains({span.first_frame, span.end_frame})) ++hits;
    }
    const auto turn_texts = clinicavt::diar::DecodeTurnTexts(turns, audio, decode, &cache);
    const auto named = clinicavt::diar::NameTurns(turns, turn_texts, result.cluster_count);
    const auto vp_start = Clock::now();
    for (int c = 0; c < result.cluster_count && c < 2; ++c) {
        (void)clinicavt::diar::ClusterVoiceprint(fed.Embedder(), audio, result.slices, c);
    }
    const double vp_s = seconds(vp_start, Clock::now());
    const double stop_s = seconds(stop_start, Clock::now());
    const std::size_t stop_decodes = decodes - capture_decodes;

    std::ptrdiff_t mismatch = -1;
    if (result.slices.size() != batch.slices.size()) {
        std::fprintf(stderr, "amortise probe: SLICE COUNT differs (%zu vs batch %zu)\n",
                     result.slices.size(), batch.slices.size());
        for (std::size_t i = 0; i < std::min(result.slices.size(), batch.slices.size()); ++i) {
            if (result.slices[i].first_frame != batch.slices[i].first_frame ||
                result.slices[i].end_frame != batch.slices[i].end_frame) {
                std::fprintf(
                    stderr, "  first divergence at %zu: fed [%.2f, %.2f) batch [%.2f, %.2f)\n", i,
                    result.slices[i].first_frame / 16000.0, result.slices[i].end_frame / 16000.0,
                    batch.slices[i].first_frame / 16000.0, batch.slices[i].end_frame / 16000.0);
                break;
            }
        }
    } else {
        mismatch = 0;
        for (std::size_t i = 0; i < result.slices.size(); ++i) {
            if (result.slices[i].first_frame != batch.slices[i].first_frame ||
                result.slices[i].end_frame != batch.slices[i].end_frame ||
                result.slices[i].cluster != batch.slices[i].cluster) {
                ++mismatch;
            }
        }
        std::fprintf(stderr, "amortise probe: slices vs batch: %td mismatched of %zu\n", mismatch,
                     result.slices.size());
    }
    std::fprintf(stderr,
                 "amortise probe: capture %.1f s over %zu ticks (%zu decodes); STOP %.2f s "
                 "(vp %.2f, %zu decodes); cache %zu speculated, %zu of %zu turns hit\n",
                 capture_s, ticks, capture_decodes, stop_s, vp_s, stop_decodes, cache.size(), hits,
                 turns.size());
    // One line per consult for the verification driver to parse
    std::fprintf(stderr,
                 "PROBE audio=%.1f capture=%.1f ticks=%zu cap_decodes=%zu stop=%.3f vp=%.3f "
                 "stop_decodes=%zu slices=%zu batch_slices=%zu mismatch=%td turns=%zu "
                 "speculated=%zu hits=%zu\n",
                 audio.size() / 16000.0, capture_s, ticks, capture_decodes, stop_s, vp_s,
                 stop_decodes, result.slices.size(), batch.slices.size(), mismatch, turns.size(),
                 cache.size(), hits);
    // The attributed transcript, for the blinded judge
    for (const auto& turn : named.turns) {
        std::printf("ATURN %s\t%s\n", turn.speaker.c_str(), turn.text.c_str());
    }
    std::error_code ec;
    std::filesystem::remove_all(anchor_root, ec);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(
            stderr,
            "usage: diar_eval_runner <models-dir> <audio.wav> [--roles] [--amortise-probe]\n");
        return 2;
    }
    bool roles = false, amortise = false;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--roles") == 0) roles = true;
        if (std::strcmp(argv[i], "--amortise-probe") == 0) {
            roles = true;
            amortise = true;
        }
    }
    try {
        const clinicavt::models::ModelStore store{std::filesystem::path(argv[1])};
        clinicavt::models::OvRuntime runtime;
        // A throwaway anchor root: evaluation must never touch a real anchor
        const auto anchor_root = std::filesystem::temp_directory_path() / "clinicavt-diar-eval";
        std::filesystem::create_directories(anchor_root);
        clinicavt::diar::AnchorStore diariser_anchors(anchor_root);
        clinicavt::diar::SpeakerDiariser diariser(store, runtime, diariser_anchors);
        const auto audio = LoadWav(argv[2]);

        std::unique_ptr<clinicavt::asr::WhisperTranscriber> whisper;
        if (roles) {
            whisper = std::make_unique<clinicavt::asr::WhisperTranscriber>(store, runtime);
        }
        const auto result = diariser.Diarise(audio);

        if (amortise) {
            AmortiseProbe(store, runtime, *whisper, audio, result);
            return 0;
        }

        if (!roles) {
            for (const auto& slice : result.slices) {
                std::printf("%.3f %.3f %d\n", static_cast<double>(slice.first_frame) / 16000.0,
                            static_cast<double>(slice.end_frame) / 16000.0, slice.cluster);
            }
            return 0;
        }

        // The production finalise: each merged turn decodes its own audio
        const auto before = std::chrono::steady_clock::now();
        const auto pturns = clinicavt::diar::MergeByCluster(result.slices);
        const auto turn_texts = clinicavt::diar::DecodeTurnTexts(
            pturns, audio, [&whisper](std::span<const float> clip, std::uint64_t first) {
                return AsChunk(Decode(*whisper, clip, first), clip, first);
            });
        const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - before);
        std::fprintf(stderr, "per-turn: %zu turns decoded in %.1f s\n", pturns.size(),
                     took.count());
        const auto named = clinicavt::diar::NameTurns(pturns, turn_texts, result.cluster_count);
        const auto& decided = named.roles;

        std::printf("DOCTOR %d\nMARGIN %.4f\n", decided.doctor_cluster, decided.margin);
        for (std::size_t c = 0; c < decided.role_of_cluster.size(); ++c) {
            std::printf("ROLE %zu %s\n", c, decided.role_of_cluster[c].c_str());
        }
        for (int c = 0; c < result.cluster_count; ++c) {
            const auto voiceprint =
                clinicavt::diar::ClusterVoiceprint(diariser.Embedder(), audio, result.slices, c);
            if (voiceprint.empty()) continue;
            std::printf("VP %d", c);
            for (const float x : voiceprint) std::printf(" %.6f", x);
            std::printf("\n");
        }
        for (const auto& slice : result.slices) {
            std::printf("SLICE %.3f %.3f %d\n", static_cast<double>(slice.first_frame) / 16000.0,
                        static_cast<double>(slice.end_frame) / 16000.0, slice.cluster);
        }
        for (const auto& turn : named.turns) {
            std::printf("TURN %s\t%s\n", turn.speaker.c_str(), turn.text.c_str());
        }

        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "diar_eval_runner: %s\n", e.what());
        return 1;
    }
}
