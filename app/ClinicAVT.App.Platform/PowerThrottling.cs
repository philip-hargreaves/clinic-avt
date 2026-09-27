using System.Runtime.Versioning;
using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.System.Threading;

namespace ClinicAVT.App.Platform;

/// <summary>
/// Windows throttles windowless background processes (EcoQoS) once the user
/// is idle, which can stretch a finalise by more than half. The engine is
/// opted out here as well as by itself, so the state never rests on one call.
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
