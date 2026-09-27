using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Shell;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>
/// Settings that would disturb a consultation being recorded or finalised wait until it is over.
/// A finished one on screen blocks nothing; the engine refuses, with its reason, anything that
/// would disturb its own work still running, such as a sheet being written.
/// </summary>
internal static class ConsultationGuard
{
    /// <summary>True, with a status line saying so, while a consultation is in progress.</summary>
    public static bool Blocks(ISessionState? session, StatusBarViewModel? status, string action)
    {
        if (session?.ConsultationInProgress != true)
        {
            return false;
        }

        status?.Append($"finish the consultation before {action}");
        return true;
    }
}
