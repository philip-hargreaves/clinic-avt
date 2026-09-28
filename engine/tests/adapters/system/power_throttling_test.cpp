#include "adapters/system/power_throttling.hpp"

#include <gtest/gtest.h>

using clinicavt::system::Describe;
using clinicavt::system::DisableThrottlingOnSelf;
using clinicavt::system::ReadThrottling;

TEST(PowerThrottling, OptOutReadsBackAsOff) {
    const auto state = DisableThrottlingOnSelf();

    ASSERT_TRUE(state.known) << "the read-back must work on this Windows";
    EXPECT_FALSE(state.defaulted) << "an explicit policy replaces Windows' own";
    EXPECT_FALSE(state.throttled);
    EXPECT_EQ(Describe(ReadThrottling(GetCurrentProcess())), "off") << "the state sticks";
}
