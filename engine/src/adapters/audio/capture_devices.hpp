#pragma once

#include <string>
#include <vector>

namespace clinicavt::audio {

// An active capture endpoint for the settings picker. id is the WASAPI
// endpoint id that WasapiCapture accepts
struct CaptureDevice {
    std::string id;
    std::string name;         // "Microphone Array (Realtek(R) Audio)"
    std::string short_name;   // "Microphone Array"
    bool is_default = false;  // the communications default, used when a pinned device is gone
    bool bluetooth = false;   // shown with the Bluetooth quality warning
};

// Not cached, so a headset plugged in after launch shows up
std::vector<CaptureDevice> ListCaptureDevices();

// A missing choice falls back to the default (caller logs it). An empty
// list gives an empty device, so capture fails
CaptureDevice ResolveMicrophone(const std::vector<CaptureDevice>& devices,
                                const std::string& requested);

std::wstring WideId(const std::string& id);

}  // namespace clinicavt::audio
