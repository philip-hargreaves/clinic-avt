#pragma once

#include <string>

// EcoQoS throttling slowed finalise 4.0 -> 6.3 s. Engine, note host and shell
// each opt out so no single call is relied on
namespace clinicavt::system {

struct ThrottlingState {
    bool known = false;      // read-back succeeded
    bool throttled = false;  // execution speed throttling is ON
    bool defaulted = true;   // no explicit policy set
};

// process is a process HANDLE
ThrottlingState ReadThrottling(void* process);

// Disables execution-speed throttling and returns the state read back afterwards
ThrottlingState DisableThrottling(void* process);

ThrottlingState DisableThrottlingOnSelf();

// "off" (opted out), "on" (throttled), "default" (Windows decides), "unknown"
std::string Describe(const ThrottlingState& state);

}  // namespace clinicavt::system
