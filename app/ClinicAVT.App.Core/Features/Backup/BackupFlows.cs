using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Features.Backup;

public static class BackupFlows
{
    /// <summary>
    /// The Back up dialog. True when it removed consultations from this computer.
    /// </summary>
    public static async Task<bool> RunBackupAsync(this IDialogService dialogs, Func<BackupViewModel> create)
    {
        using var backup = create();
        _ = backup.LoadAsync();
        await dialogs.ShowAsync(backup).ConfigureAwait(true);
        return backup.RemovedAny;
    }

    /// <summary>The Restore dialog. True when it added consultations.</summary>
    public static async Task<bool> RunRestoreAsync(this IDialogService dialogs, Func<RestoreViewModel> create)
    {
        using var restore = create();
        await dialogs.ShowAsync(restore).ConfigureAwait(true);
        return restore.RestoredAny;
    }
}
