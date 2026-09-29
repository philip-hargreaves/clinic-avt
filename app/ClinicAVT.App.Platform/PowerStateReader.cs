using Microsoft.Win32;
using Windows.Win32;
using Windows.Win32.System.Power;
using ClinicAVT.App.Core.Metrics;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Platform;

/// <summary>The power slider's overlay from the registry and the mains state from Win32.</summary>
public sealed class PowerStateReader : IPowerStateReader
{
    private const string OverlayKey =
        @"SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes";

    public PowerState Read()
    {
        var onMains = !PInvoke.GetSystemPowerStatus(out var status) || status.ACLineStatus != 0;

        var overlay = "";
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(OverlayKey);
            overlay = key?.GetValue(onMains ? "ActiveOverlayAcPowerScheme" : "ActiveOverlayDcPowerScheme")
                as string ?? "";
        }
        catch (Exception)
        {
            // An unreadable key reads as the default overlay
        }

        return new PowerState(PowerState.ModeName(overlay), onMains);
    }
}
