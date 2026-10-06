#include "adapters/audio/capture_errors.hpp"

#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <audioclient.h>
#include <windows.h>

namespace clinicavt::audio {
namespace {

// The header's constants must match the SDK's, checked at compile time
static_assert(hresults::kDeviceInvalidated ==
              static_cast<std::uint32_t>(AUDCLNT_E_DEVICE_INVALIDATED));
static_assert(hresults::kServiceNotRunning ==
              static_cast<std::uint32_t>(AUDCLNT_E_SERVICE_NOT_RUNNING));
static_assert(hresults::kEndpointCreateFailed ==
              static_cast<std::uint32_t>(AUDCLNT_E_ENDPOINT_CREATE_FAILED));
static_assert(hresults::kResourcesInvalidated ==
              static_cast<std::uint32_t>(AUDCLNT_E_RESOURCES_INVALIDATED));
static_assert(hresults::kAccessDenied == static_cast<std::uint32_t>(E_ACCESSDENIED));

TEST(EndForCaptureError, ClassifiesDeviceLossConsentAndOtherFailures) {
    const std::uint32_t device_gone[] = {
        hresults::kDeviceInvalidated,
        hresults::kServiceNotRunning,
        hresults::kEndpointCreateFailed,
        hresults::kResourcesInvalidated,
    };
    for (const auto hr : device_gone) {
        const auto end = EndForCaptureError("GetBuffer", hr);
        EXPECT_EQ(end.reason, SourceEndReason::kDeviceLost) << end.detail;
        EXPECT_NE(end.detail.find("GetBuffer"), std::string::npos) << end.detail;
    }

    // A consent refusal tells the clinician where to grant microphone access
    const auto denied = EndForCaptureError("Initialize", hresults::kAccessDenied);
    EXPECT_EQ(denied.reason, SourceEndReason::kFailed);
    EXPECT_NE(denied.detail.find("privacy settings"), std::string::npos) << denied.detail;
    EXPECT_NE(denied.detail.find("0x80070005"), std::string::npos) << denied.detail;

    const auto other = EndForCaptureError("Activate", 0x8007000E);  // E_OUTOFMEMORY
    EXPECT_EQ(other.reason, SourceEndReason::kFailed);
    EXPECT_NE(other.detail.find("Activate"), std::string::npos) << other.detail;
    EXPECT_NE(other.detail.find("0x8007000E"), std::string::npos) << other.detail;
}

}  // namespace
}  // namespace clinicavt::audio
