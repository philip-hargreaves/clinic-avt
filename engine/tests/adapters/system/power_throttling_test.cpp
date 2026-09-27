#include "adapters/system/power_throttling.hpp"

#include <gtest/gtest.h>

using clinicavt::system::Describe;
using clinicavt::system::DisableThrottlingOnSelf;
using clinicavt::system::ReadThrottling;

TEST(PowerThrottling, OptOutReadsBackAsOffAndEveryStateHasAName) {
    const auto state = DisableThrottlingOnSelf();

    ASSERT_TRUE(state.known) << "the read-back must work on this Windows";
    EXPECT_FALSE(state.defaulted) << "an explicit policy replaces Windows' own";
    EXPECT_FALSE(state.throttled);
    EXPECT_EQ(Describe(ReadThrottling(GetCurrentProcess())), "off") << "the state sticks";

    EXPECT_EQ(Describe({}), "unknown");
    EXPECT_EQ(Describe({.known = true, .throttled = false, .defaulted = true}), "default");
    EXPECT_EQ(Describe({.known = true, .throttled = true, .defaulted = false}), "on");
    EXPECT_EQ(Describe({.known = true, .throttled = false, .defaulted = false}), "off");
}
