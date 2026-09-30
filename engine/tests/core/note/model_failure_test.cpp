#include "core/note/model_failure.hpp"

#include <gtest/gtest.h>

namespace clinicavt::note {
namespace {

// The texts are OpenVINO's own, as measured on the Arc 140T
TEST(ModelFailure, ALoadFailureBecomesAPlainMessageNamingTheModel) {
    EXPECT_EQ(ClassifyLoadFailure("[CL ext] Can not allocate 134217728 bytes for USM Host. ptr: "
                                  "0000000000000000, error: -6"),
              LoadFailure::kMemory);
    EXPECT_EQ(ClassifyLoadFailure("bad allocation"), LoadFailure::kMemory);
    EXPECT_EQ(ClassifyLoadFailure("[GPU] ProgramBuilder build failed! [GPU] Failed to create "
                                  "program during kernel build process"),
              LoadFailure::kCache);
    EXPECT_EQ(ClassifyLoadFailure("no such device"), LoadFailure::kOther);

    EXPECT_EQ(PlainLoadMessage(LoadFailure::kMemory, "Qwen3.6 35B"),
              "There is not enough free memory to load Qwen3.6 35B. Close other programs or free "
              "some disk space, or choose a smaller model");
    EXPECT_EQ(PlainLoadMessage(LoadFailure::kOther, ""), "the note model could not be loaded");
}

TEST(ModelFailure, OnlyADriverFaultPoisonsTheProcess) {
    EXPECT_TRUE(PoisonsGpuContext("clEnqueueNDRangeKernel, error code: -5 CL_OUT_OF_RESOURCES"));
    EXPECT_TRUE(PoisonsGpuContext("could not execute a primitive"));
    EXPECT_FALSE(PoisonsGpuContext("Can not allocate 11796480 bytes for USM Host. error: -6"))
        << "a refused allocation leaves the context usable";
}

}  // namespace
}  // namespace clinicavt::note
