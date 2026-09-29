#include "adapters/system/power_throttling.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace clinicavt::system {

ThrottlingState ReadThrottling(void* process) {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    ThrottlingState out;
    if (!GetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state))) {
        return out;
    }
    out.known = true;
    out.defaulted = (state.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) == 0;
    out.throttled = (state.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0;
    return out;
}

ThrottlingState DisableThrottling(void* process) {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    SetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state));
    return ReadThrottling(process);
}

ThrottlingState DisableThrottlingOnSelf() {
    return DisableThrottling(GetCurrentProcess());
}

std::string Describe(const ThrottlingState& state) {
    if (!state.known) return "unknown";
    if (!state.defaulted) return state.throttled ? "on" : "off";
    return "default";
}

}  // namespace clinicavt::system
