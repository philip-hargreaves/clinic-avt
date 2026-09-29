using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>
/// Blocks settings changes that would disturb a consultation being recorded or finalised. The
/// engine refuses other clashes itself.
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
