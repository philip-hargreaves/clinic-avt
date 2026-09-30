using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Common;

public static class SessionDeletion
{
    /// <summary>
    /// Confirms, then runs closeReview and deletes the session. True when it was deleted.
    /// </summary>
    public static async Task<bool> ConfirmAndDeleteAsync(
        IDialogService dialogs, ISessionStoreApi engine, IStatusLine status, string id, Func<Task> closeReview)
    {
        // Deletion is crypto-erase, so it is confirmed first
        if (!await dialogs.ConfirmAsync("Delete this consultation?",
                "The transcript, note and patient information are erased and cannot be recovered.",
                "Delete", "Keep").ConfigureAwait(true))
        {
            return false;
        }

        return await EngineCall.ReportAsync(status, "could not delete consultation", async () =>
        {
            await closeReview().ConfigureAwait(true);
            await engine.DeleteSessionAsync(id).ConfigureAwait(true);
            status.Append("Consultation deleted");
        }).ConfigureAwait(true);
    }
}
