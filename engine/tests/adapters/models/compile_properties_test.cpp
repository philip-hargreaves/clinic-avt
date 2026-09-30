#include <gtest/gtest.h>

#include "adapters/models/ov_runtime.hpp"

namespace clinicavt::models {
namespace {

TEST(CompileProperties, TheCacheDirAndManifestPropertiesPassThrough) {
    ModelInfo info;
    info.dir = std::filesystem::path("C:/models/nllb");
    EXPECT_TRUE(CompileProperties(info).empty()) << "no cache dir and no properties";

    // A non-ASCII profile name reaches OpenVINO as UTF-8
    info.cache_dir =
        std::filesystem::path(u8"C:/Users/Zo\u00EB/AppData/Local/ClinicAVT/cache/nllb");
    EXPECT_EQ(CompileProperties(info).size(), 1u) << "no properties: the cache alone";

    info.properties = {
        {"CACHE_MODE", "OPTIMIZE_SIZE"}, {"ACTIVATIONS_SCALE_FACTOR", 32}, {"ENABLE_MMAP", true}};
    const ov::AnyMap map = CompileProperties(info);

    EXPECT_EQ(map.at("CACHE_DIR").as<std::string>(),
              "C:/Users/Zo\xC3\xAB/AppData/Local/ClinicAVT/cache/nllb");
    EXPECT_EQ(map.at("CACHE_MODE").as<std::string>(), "OPTIMIZE_SIZE");
    EXPECT_EQ(map.at("ACTIVATIONS_SCALE_FACTOR").as<std::string>(), "32");
    EXPECT_TRUE(map.at("ENABLE_MMAP").as<bool>());
}

}  // namespace
}  // namespace clinicavt::models
