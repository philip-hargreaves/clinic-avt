#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/translate/nllb_translator.hpp"

namespace clinicavt::translate {
namespace {

const std::filesystem::path kModels = CLINICAVT_MODELS_DIR;

constexpr const char* kSheet =
    "Your appointment today\n"
    "You came to see us about a swelling on your left elbow. "
    "It started about a week ago. It is not painful, but it feels warm.\n"
    "Your treatment and next steps\n"
    "Take ibuprofen 400mg twice a day, after food. Stop taking it if you get heartburn.";

TEST(NllbTranslator, TranslatesStreamsAndSurvivesARelease) {
    if (!std::filesystem::exists(kModels / "nllb-200-600m-int8")) {
        GTEST_SKIP() << "translation model not staged";
    }
    models::ModelStore store(kModels);
    models::OvRuntime runtime;
    NllbTranslator translator(store, runtime);
    EXPECT_FALSE(translator.Languages().empty());
    translator.Prepare();

    int partials = 0;
    const std::string french =
        translator.Translate(kSheet, "French", [&partials](const std::string&) { partials++; });
    ASSERT_FALSE(french.empty());
    EXPECT_NE(french, kSheet);
    EXPECT_GT(partials, 2) << "the translation streams";
    EXPECT_NE(french.find("ibuprof"), std::string::npos) << french;

    const std::string polish = translator.Translate(kSheet, "Polish", nullptr);
    ASSERT_FALSE(polish.empty());
    EXPECT_NE(polish, french);

    EXPECT_EQ(
        translator.Translate("It\xE2\x80\x99s your body\xE2\x80\x99s defence.", "Urdu", nullptr),
        translator.Translate("It's your body's defence.", "Urdu", nullptr))
        << "a curly apostrophe translates as a straight one";

    std::string edited = kSheet;
    std::replace(edited.begin(), edited.end(), '\n', '\r');
    EXPECT_EQ(translator.Translate(edited, "French", nullptr), french)
        << "a sheet saved from a text box, with CR line ends, translates line by line";

    EXPECT_THROW(translator.Translate("hello", "Klingon", nullptr), std::runtime_error);

    const std::string before = translator.Translate(kSheet, "Punjabi", nullptr);
    translator.Release();
    // Release returns before the unload
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(translator.Translate(kSheet, "Punjabi", nullptr), before)
        << "a released translator loads again and translates the same";
}

}  // namespace
}  // namespace clinicavt::translate
