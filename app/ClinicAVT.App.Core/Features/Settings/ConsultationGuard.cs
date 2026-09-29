using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>
/// Settings that would disturb a consultation being recorded or finalised wait until it is over.
/// The engine refuses, with its reason, anything that clashes with its own work still running.
/// </summary>
internal static class ConsultationGuard
{
    /// <summary>True, with a status line saying so, while a consultation is in progress.</summary>
    public static bool Blocks(ISessionState session, IStatusLine status, string action)
    {
        if (!session.ConsultationInProgress)
        {
            return false;
        }

        status.Append($"finish the consultation before {action}");
        return true;
    }
}
