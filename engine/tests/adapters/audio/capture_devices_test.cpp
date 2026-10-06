#include "adapters/audio/capture_devices.hpp"

#include <gtest/gtest.h>

namespace {

// Real enumeration. A runner with no microphone lists nothing
TEST(CaptureDevices, EveryListedDeviceIsWellFormed) {
    const auto devices = clinicavt::audio::ListCaptureDevices();
    int defaults = 0;
    for (const auto& device : devices) {
        EXPECT_FALSE(device.id.empty());
        EXPECT_FALSE(device.name.empty());
        EXPECT_FALSE(device.short_name.empty());
        defaults += device.is_default ? 1 : 0;
    }
    EXPECT_LE(defaults, 1) << "at most one communications default";
    if (!devices.empty()) {
        EXPECT_EQ(defaults, 1) << "a non-empty list names its default";
    }
}

TEST(ResolveMicrophone, PicksTheChoiceElseTheDefaultElseTheFirst) {
    using clinicavt::audio::ResolveMicrophone;
    const std::vector<clinicavt::audio::CaptureDevice> devices{
        {"{aa}", "USB Mic", "USB Mic", false, false},
        {"{bb}", "Array", "Array", true, false},
        {"{cc}", "Headset", "Headset", false, true},
    };
    EXPECT_EQ(ResolveMicrophone(devices, "{cc}").id, "{cc}") << "the requested device";

    const auto gone = ResolveMicrophone(devices, "{unplugged}");
    EXPECT_EQ(gone.id, "{bb}") << "an unplugged choice falls to the default";
    EXPECT_EQ(gone.name, "Array");

    const std::vector<clinicavt::audio::CaptureDevice> no_default{
        {"{aa}", "USB Mic", "USB Mic", false, false},
    };
    EXPECT_EQ(ResolveMicrophone(no_default, "").id, "{aa}") << "no default falls to the first";
    EXPECT_EQ(ResolveMicrophone({}, "{any}").id, "") << "nothing to pick";
}

}  // namespace
