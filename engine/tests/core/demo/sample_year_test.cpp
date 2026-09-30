#include "core/demo/sample_year.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "core/common/iso8601.hpp"

namespace clinicavt::demo {
namespace {

using namespace std::chrono;

TEST(SampleYear, StartsFallOnTheSampleDayMonthsBackClampedToTheMonth) {
    Sample sample;
    sample.months_back = 1;
    sample.day = 31;
    sample.hour = 13;
    sample.minute = 15;
    // 15 March 2026 -> February has 28 days, so the 31st becomes the 28th
    const sys_seconds now = sys_days{2026y / March / 15} + hours{10};
    EXPECT_EQ(Iso8601(SampleStart(sample, now)), "2026-02-28T13:15:00Z");

    sample.months_back = 11;
    sample.day = 4;
    EXPECT_EQ(Iso8601(SampleStart(sample, now)), "2025-04-04T13:15:00Z");
}

TEST(SampleYear, ASampleInTheCurrentMonthIsNeverDatedAfterNow) {
    Sample sample;
    sample.months_back = 0;
    sample.day = 8;
    sample.hour = 10;
    sample.minute = 30;
    const sys_seconds now = sys_days{2026y / September / 30} + hours{13};
    EXPECT_EQ(Iso8601(SampleStart(sample, now)), "2026-09-08T10:30:00Z");

    const sys_seconds early = sys_days{2026y / October / 5} + hours{9};
    EXPECT_EQ(Iso8601(SampleStart(sample, early)), "2026-09-08T10:30:00Z") << "8 October is ahead";
    sample.day = 5;
    EXPECT_EQ(Iso8601(SampleStart(sample, early)), "2026-09-05T10:30:00Z")
        << "10:30 is ahead of 09:00";
    sample.day = 4;
    EXPECT_EQ(Iso8601(SampleStart(sample, early)), "2026-10-04T10:30:00Z");
}

}  // namespace
}  // namespace clinicavt::demo
