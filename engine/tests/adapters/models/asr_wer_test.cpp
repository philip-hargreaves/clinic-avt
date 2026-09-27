#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/diarisation/speaker_diariser.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/transcription/whisper_transcriber.hpp"
#include "core/diarisation/turn_decode.hpp"
#include "ports/audio_source.hpp"

namespace clinicavt::asr {
namespace {

// Not in the repo. The test skips without it
constexpr const char* kWav =
    "C:/dev/intelliscribe/bench/transcription/mixed/day1_consultation01_mixed.wav";
constexpr const char* kRef =
    "C:/dev/intelliscribe/bench/transcription/references/day1_consultation01.json";

// Long-form parity on this consult is 21.28%. The shipped per-turn decode
// (each diarised turn from its own audio) measured under it on the
// 57-consult sweep, so the gate holds the baseline
constexpr double kMaxWer = 0.22;

std::vector<float> LoadWav(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error(std::string("missing dev wav: ") + path);
    in.seekg(0, std::ios::end);
    const auto bytes = static_cast<std::size_t>(in.tellg()) - 44;
    in.seekg(44);
    std::vector<std::int16_t> pcm(bytes / 2);
    in.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(bytes));
    std::vector<float> frames(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) frames[i] = pcm[i] / 32768.0f;
    return frames;
}

std::string LoadGold(const char* path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error(std::string("missing reference: ") + path);
    const auto ref = nlohmann::json::parse(in);
    std::vector<std::pair<double, std::string>> turns;
    for (const char* role : {"doctor", "patient"}) {
        for (const auto& turn : ref.at(role)) {
            const auto text = turn.value("text", "");
            if (!text.empty()) turns.push_back({turn.at("start").get<double>(), text});
        }
    }
    std::sort(turns.begin(), turns.end());
    std::string joined;
    for (const auto& [start, text] : turns) joined += text + " ";
    return joined;
}

// Same normalisation as the research scorer: lowercase, keep [a-z0-9'],
// collapse whitespace
std::vector<std::string> NormalisedWords(const std::string& text) {
    std::string cleaned;
    for (const char c : text) {
        const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        cleaned += (std::isalnum(static_cast<unsigned char>(lower)) || lower == '\'') ? lower : ' ';
    }
    std::vector<std::string> words;
    std::istringstream stream(cleaned);
    std::string word;
    while (stream >> word) words.push_back(word);
    return words;
}

double Wer(const std::vector<std::string>& ref, const std::vector<std::string>& hyp) {
    std::vector<std::size_t> previous(hyp.size() + 1);
    std::vector<std::size_t> current(hyp.size() + 1);
    for (std::size_t j = 0; j <= hyp.size(); ++j) previous[j] = j;
    for (std::size_t i = 1; i <= ref.size(); ++i) {
        current[0] = i;
        for (std::size_t j = 1; j <= hyp.size(); ++j) {
            const std::size_t substitution = previous[j - 1] + (ref[i - 1] == hyp[j - 1] ? 0 : 1);
            current[j] = std::min({substitution, previous[j] + 1, current[j - 1] + 1});
        }
        std::swap(previous, current);
    }
    return static_cast<double>(previous[hyp.size()]) / static_cast<double>(ref.size());
}

TEST(AsrWer, ProductionPathHoldsTheBaseline) {
    if (!std::filesystem::exists(kWav) || !std::filesystem::exists(kRef)) {
        GTEST_SKIP() << "research corpus not mounted";
    }
    const auto frames = LoadWav(kWav);
    const auto gold = NormalisedWords(LoadGold(kRef));

    const models::ModelStore store(std::filesystem::path(CLINICAVT_MODELS_DIR));
    models::OvRuntime runtime;
    const auto load_start = std::chrono::steady_clock::now();
    WhisperTranscriber transcriber(store, runtime);
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - load_start);
    // A throwaway anchor root: evaluation must never touch a real anchor
    const auto anchor_root = std::filesystem::temp_directory_path() / "clinicavt-asr-wer-anchor";
    std::filesystem::create_directories(anchor_root);
    diar::AnchorStore anchors(anchor_root);
    diar::SpeakerDiariser diariser(store, runtime, anchors);

    // The production finalise: each diarised turn decodes its own audio
    const auto decode_start = std::chrono::steady_clock::now();
    const auto result = diariser.Diarise(frames);
    const auto turns = diar::MergeByCluster(result.slices);
    // Whisper pads its window to 30 s, so its stamps can overrun a clip. Every
    // chunk must still land inside its turn's clip (a second of slack)
    std::size_t outside = 0;
    const auto texts = diar::DecodeTurnTexts(
        turns, frames, [&transcriber, &outside](std::span<const float> clip, std::uint64_t first) {
            auto chunks = transcriber.DecodeClipChunks(clip, first);
            for (const auto& chunk : chunks) {
                if (chunk.text.empty() || chunk.first_frame < first ||
                    chunk.first_frame + chunk.frame_count >
                        first + clip.size() + audio::kSampleRate) {
                    ++outside;
                }
            }
            return chunks;
        });
    const auto decode =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - decode_start);

    std::string joined;
    for (const auto& text : texts) joined += text + " ";
    const auto hyp = NormalisedWords(joined);
    const double wer = Wer(gold, hyp);
    const double speed = (static_cast<double>(frames.size()) / audio::kSampleRate) / decode.count();

    std::printf("per-turn WER %.2f%% (baseline 21.28%%), %.1fx realtime, load %lld ms, %zu turns\n",
                wer * 100, speed, static_cast<long long>(load_ms.count()), turns.size());
    EXPECT_LE(wer, kMaxWer);
    EXPECT_EQ(outside, 0u) << "chunks empty or outside their clip";
    // Release measures well past realtime. The floor tolerates the Debug harness
    // and the diarisation inside the timed span
    EXPECT_GT(speed, 3.0) << "decode must stay well past realtime";
    std::error_code ec;
    std::filesystem::remove_all(anchor_root, ec);
}

}  // namespace
}  // namespace clinicavt::asr
