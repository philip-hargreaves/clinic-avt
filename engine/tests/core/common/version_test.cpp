#include "core/common/version.hpp"

#include <gtest/gtest.h>

// version.hpp is kept by hand, so it must follow the CMake project version
TEST(Version, MatchesCMakeProjectVersion) {
    EXPECT_STREQ(clinicavt::kVersion, CLINICAVT_CMAKE_VERSION);
}
