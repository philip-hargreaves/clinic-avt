#include "core/metrics/throughput.hpp"

#include <gtest/gtest.h>

namespace clinicavt::metrics {
namespace {

// A zero span must read as 0 so no inf or NaN reaches the wire
TEST(Throughput, TheRateFollowsTheStreamAndTheAverageTheWholeGeneration) {
    ThroughputMeter meter;
    EXPECT_EQ(meter.Rate(1.0), 0);
    EXPECT_EQ(meter.Average(), 0);
    meter.Token(0.0);
    EXPECT_EQ(meter.Rate(0.5), 0) << "one token is not a rate";
    EXPECT_EQ(meter.Average(), 0);
    ThroughputMeter same;
    same.Token(2.0);
    same.Token(2.0);  // same stamp, so no span
    EXPECT_EQ(same.Average(), 0);

    for (int i = 1; i <= 20; ++i) {
        meter.Token(i * 0.1);  // 10 tok/s for two seconds
    }
    EXPECT_NEAR(meter.Rate(2.0), 10.0, 0.5);
    EXPECT_NEAR(meter.Average(), 10.0, 0.5);

    for (int i = 1; i <= 50; ++i) {
        meter.Token(2.0 + i * 0.02);  // then 50 tok/s for one second
    }
    EXPECT_GT(meter.Rate(3.1), 25.0) << "the window rolls";
    EXPECT_NEAR(meter.Average(), 70.0 / 3.0, 0.5);

    EXPECT_EQ(meter.Rate(8.0), 0) << "a stall decays the rate";
    EXPECT_NEAR(meter.Average(), 70.0 / 3.0, 0.5) << "but not the average";
}

}  // namespace
}  // namespace clinicavt::metrics
