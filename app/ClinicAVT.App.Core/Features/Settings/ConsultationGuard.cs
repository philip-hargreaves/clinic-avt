using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Shell;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>Settings that would disturb a live consultation wait until it is over.</summary>
internal static class ConsultationGuard
{
    /// <summary>True, with a status line saying so, while a consultation is active.</summary>
    public static bool Blocks(ISessionState? session, StatusBarViewModel? status, string action)
    {
        if (session?.ConsultationActive != true)
        {
            return false;
        }

        status?.Append($"finish the consultation before {action}");
        return true;
    }
}
