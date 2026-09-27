#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapters/diarisation/segmenter.hpp"
#include "dev_wav.hpp"

namespace clinicavt::diar {
namespace {

// Research check D: per-frame powerset argmax agreement is the faithfulness
// criterion, since the decoded class is the only downstream consumer. Raw
// log-probs carry harmless bi-LSTM float noise. The decode gate then holds
// the engine's change points and overlap spans against the reference decode
// of the same windows, tolerant to one ~17 ms frame of drift
constexpr const char* kFixtureDir = CLINICAVT_DIAR_FIXTURE_DIR;
constexpr std::uint64_t kFrameTolerance = 300;  // one seg frame is ~272 samples

nlohmann::json LoadMeta() {
    std::ifstream in(std::filesystem::path(kFixtureDir) / "seg_fixtures.json");
    if (!in.is_open()) throw std::runtime_error("missing segmentation fixtures");
    return nlohmann::json::parse(in);
}

TEST(SegModel, MatchesTheResearchArgmaxAndReferenceDecode) {
    const auto meta = LoadMeta();
    // The fixture names a wav that is not in the repo
    if (!std::filesystem::exists(meta.at("wav").get<std::string>())) {
        GTEST_SKIP() << "research corpus not mounted";
    }
    const auto audio = LoadDevWav(meta.at("wav"));
    const std::size_t windows = meta.at("windows");
    const std::size_t frames = meta.at("frames_per_window");

    std::vector<std::int8_t> reference(windows * frames);
    std::ifstream bin(
        std::filesystem::path(kFixtureDir) / meta.at("classes_file").get<std::string>(),
        std::ios::binary);
    bin.read(reinterpret_cast<char*>(reference.data()),
             static_cast<std::streamsize>(reference.size()));
    ASSERT_TRUE(bin.good());

    const models::ModelStore store{std::filesystem::path(CLINICAVT_MODELS_DIR)};
    models::OvRuntime runtime;
    {
        auto loaded = runtime.Load(store, "segmentation", "default", "model.onnx");
        EXPECT_EQ(loaded.device, "CPU");
        auto request = loaded.model.create_infer_request();

        std::size_t agree = 0;
        ov::Tensor input(ov::element::f32, {1, 1, kSegWindowFrames});
        for (std::size_t w = 0; w < windows; ++w) {
            float* x = input.data<float>();
            const std::uint64_t offset = w * kSegWindowFrames;
            const std::size_t have = std::min<std::size_t>(kSegWindowFrames, audio.size() - offset);
            std::copy_n(audio.data() + offset, have, x);
            std::fill(x + have, x + kSegWindowFrames, 0.0f);
            request.set_input_tensor(input);
            request.infer();
            const ov::Tensor output = request.get_output_tensor();
            const float* logits = output.data<float>();
            const std::size_t classes = output.get_shape()[2];
            for (std::size_t i = 0; i < frames; ++i) {
                const float* row = logits + i * classes;
                const auto argmax = std::max_element(row, row + classes) - row;
                if (argmax == reference[w * frames + i]) ++agree;
            }
        }
        const double agreement = static_cast<double>(agree) / static_cast<double>(windows * frames);
        std::printf("seg argmax agreement %.4f%% (%zu/%zu frames)\n", agreement * 100, agree,
                    windows * frames);
        EXPECT_GE(agreement, 0.9999);
    }

    // The decode over the same audio holds the reference change points and overlap spans
    Segmenter segmenter(store, runtime);
    const auto result = segmenter.Run(audio);

    const auto ref_changes = meta.at("change_points").get<std::vector<std::uint64_t>>();
    ASSERT_EQ(result.change_points.size(), ref_changes.size());
    for (std::size_t i = 0; i < ref_changes.size(); ++i) {
        const auto delta = result.change_points[i] > ref_changes[i]
                               ? result.change_points[i] - ref_changes[i]
                               : ref_changes[i] - result.change_points[i];
        EXPECT_LE(delta, kFrameTolerance) << "change point " << i;
    }

    const auto ref_spans = meta.at("overlap_spans").get<std::vector<std::vector<std::uint64_t>>>();
    ASSERT_EQ(result.overlap_spans.size(), ref_spans.size());
    for (std::size_t i = 0; i < ref_spans.size(); ++i) {
        EXPECT_NEAR(static_cast<double>(result.overlap_spans[i].first_frame),
                    static_cast<double>(ref_spans[i][0]), static_cast<double>(kFrameTolerance))
            << "overlap span " << i;
        EXPECT_NEAR(static_cast<double>(result.overlap_spans[i].end_frame),
                    static_cast<double>(ref_spans[i][1]), static_cast<double>(kFrameTolerance))
            << "overlap span " << i;
    }
}

}  // namespace
}  // namespace clinicavt::diar
