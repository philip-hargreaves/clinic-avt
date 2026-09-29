using System.Runtime.Versioning;
using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.System.Threading;

namespace ClinicAVT.App.Platform;

/// <summary>
/// Opts a process out of EcoQoS, which can slow a finalise by over half when the user is idle.
/// The engine also does this itself.
/// </summary>
[SupportedOSPlatform("windows8.0")]
public static class PowerThrottling
{
    public static unsafe bool Disable(SafeProcessHandle process)
    {
        var state = new PROCESS_POWER_THROTTLING_STATE
        {
            Version = PInvoke.PROCESS_POWER_THROTTLING_CURRENT_VERSION,
            ControlMask = PInvoke.PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
            StateMask = 0,
        };
        return PInvoke.SetProcessInformation(
            new HANDLE(process.DangerousGetHandle()),
            PROCESS_INFORMATION_CLASS.ProcessPowerThrottling, &state,
            (uint)sizeof(PROCESS_POWER_THROTTLING_STATE));
    }
}
