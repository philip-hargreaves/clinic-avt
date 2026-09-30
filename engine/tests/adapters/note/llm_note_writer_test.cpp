#include "adapters/note/llm_note_writer.hpp"

#include <gtest/gtest.h>

#include <filesystem>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/system/gpu_lease.hpp"

namespace clinicavt::note {
namespace {

const std::filesystem::path kModels = CLINICAVT_MODELS_DIR;

std::vector<asr::Turn> ElbowTranscript() {
    return {{0, 16000, "doctor", "What seems to be the problem today?"},
            {16000, 32000, "patient",
             "I noticed a swelling on my left elbow about a week ago. It is not painful, "
             "just slightly warm, and it feels like there is fluid inside."},
            {48000, 16000, "doctor", "Have you injured that elbow at all?"},
            {64000, 16000, "patient", "No, not that I know of."},
            {80000, 32000, "doctor",
             "This looks like bursitis. I would take ibuprofen, four hundred milligrams "
             "twice a day after food, and we will arrange blood tests."}};
}

// The only real-model proof that cancel stops a generation. Runs in Debug too
TEST(LlmNoteWriter, CancelInterruptsAGeneration) {
    if (!std::filesystem::exists(kModels / "qwen3.5-9b-int4")) {
        GTEST_SKIP() << "note model not staged";
    }
    models::ModelStore store(kModels);
    models::OvRuntime runtime;
    system::GpuLease gpu(system::InheritedGpuLeaseName());
    LlmNoteWriter writer(store, runtime, gpu, kModels.parent_path() / "prompts");

    int seen = 0;
    writer.Write(ElbowTranscript(), {}, [&writer, &seen](const std::string&) {
        if (++seen == 3) writer.Cancel();
    });

    EXPECT_GE(seen, 3);
    EXPECT_LT(seen, 40) << "cancel must stop generation promptly";
}

}  // namespace
}  // namespace clinicavt::note
