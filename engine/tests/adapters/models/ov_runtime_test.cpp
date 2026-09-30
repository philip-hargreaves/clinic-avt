#include "adapters/models/ov_runtime.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <openvino/op/constant.hpp>
#include <openvino/op/multiply.hpp>
#include <openvino/op/parameter.hpp>
#include <string>

#include "adapters/system/sha256.hpp"

namespace clinicavt::models {
namespace {

struct TempRoot {
    std::filesystem::path path;

    TempRoot() {
        path =
            std::filesystem::temp_directory_path() /
            ("clinicavt-ovrt-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::create_directories(path);
    }

    ~TempRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

// A model with known behaviour (doubles 8 floats), synthesised so the test
// depends on no external weights
void SynthesiseModel(const std::filesystem::path& dir, const std::string& device) {
    std::filesystem::create_directories(dir);

    auto input = std::make_shared<ov::op::v0::Parameter>(ov::element::f32, ov::Shape{1, 8});
    auto two = ov::op::v0::Constant::create(ov::element::f32, ov::Shape{1, 1}, {2.0F});
    auto doubled = std::make_shared<ov::op::v1::Multiply>(input, two);
    const auto model =
        std::make_shared<ov::Model>(ov::OutputVector{doubled}, ov::ParameterVector{input});
    ov::serialize(model, (dir / "model.xml").string(), (dir / "model.bin").string());

    // Sizes as the shipped manifests carry them. The load check is the size
    std::ofstream manifest(dir / "manifest.json");
    manifest << "{\"manifestVersion\": 1, \"id\": \"selftest\", \"task\": \"selftest\","
             << " \"tier\": \"default\", \"licence\": \"MIT\","
             << " \"runtime\": {\"device\": \"" << device << "\"}, \"files\": {"
             << "\"model.xml\": \"" << system::Sha256File(dir / "model.xml") << "\","
             << "\"model.bin\": \"" << system::Sha256File(dir / "model.bin") << "\"}, \"bytes\": {"
             << "\"model.xml\": " << std::filesystem::file_size(dir / "model.xml") << ","
             << "\"model.bin\": " << std::filesystem::file_size(dir / "model.bin") << "}}";
}

TEST(OvRuntime, LoadsVerifiedModelsOnTheIntelGpuAndRefusesATamperedOne) {
    TempRoot root;
    SynthesiseModel(root.path / "selftest", "GPU");

    const ModelStore store(root.path);
    OvRuntime runtime;
    {
        LoadedModel loaded = runtime.Load(store, "selftest", "default", "model.xml");
        EXPECT_EQ(loaded.device.rfind("GPU", 0), 0u) << loaded.device;

        ov::InferRequest request = loaded.model.create_infer_request();
        ov::Tensor in(ov::element::f32, {1, 8});
        for (int i = 0; i < 8; ++i) in.data<float>()[i] = static_cast<float>(i + 1);
        request.set_input_tensor(in);
        request.infer();

        const ov::Tensor out = request.get_output_tensor();
        ASSERT_EQ(out.get_size(), 8u);
        for (int i = 0; i < 8; ++i) {
            EXPECT_FLOAT_EQ(out.data<const float>()[i], 2.0F * static_cast<float>(i + 1));
        }
    }

    // Released first, so nothing still maps the weights
    const auto bin = root.path / "selftest" / "model.bin";
    const auto size = std::filesystem::file_size(bin);
    std::ofstream(bin, std::ios::binary | std::ios::app) << "x";
    ASSERT_EQ(std::filesystem::file_size(bin), size + 1);
    try {
        runtime.Load(store, "selftest", "default", "model.xml");
        FAIL() << "a tampered model must be refused";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("model.bin"), std::string::npos) << e.what();
    }
}

}  // namespace
}  // namespace clinicavt::models
