using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Shell;

/// <summary>
/// Confirms if recording, saves review edits, releases the engine and closes the connection,
/// in that order. The engine exits on its own after any note model load, so no GPU work is
/// killed.
/// </summary>
internal sealed class AppShutdown(
    IConsultation session, IDialogService dialogs, EngineConnection connection,
    IEngineHost host, ILogger<AppShutdown> logger)
{
    /// <summary>False when the clinician chose to keep recording.</summary>
    public async Task<bool> ConfirmAsync() =>
        session.State is not (SessionState.Recording or SessionState.Finalising)
        || await dialogs.ConfirmAsync(
            "Close during a consultation?",
            "The recording so far is kept; the note will not be written.", "Close", "Keep recording");

    public async Task StopAsync()
    {
        try
        {
            await session.CloseReviewAsync();
            await host.ReleaseAsync();
            await connection.DisposeAsync();
        }
        catch (Exception e)
        {
            logger.ShutdownStepFailed(e);
        }
    }
}
