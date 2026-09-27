namespace ClinicAVT.App.Core.Ports;

/// <summary>
/// The dialogs the shell shows on a view model's behalf. Confirmations default
/// to cancel. The two flows own their view model for the dialog's lifetime.
/// </summary>
public interface IDialogService
{
    /// <summary>True when the primary button was chosen. The other button cancels.</summary>
    Task<bool> ConfirmAsync(string title, string content, string primary, string cancel = "Cancel");

    /// <summary>
    /// A confirmation with one tick box, unticked at first. Null when cancelled, otherwise
    /// whether the box was ticked.
    /// </summary>
    Task<bool?> ConfirmWithOptionAsync(
        string title, string content, string tick, string primary, string cancel = "Cancel");

    /// <summary>The Back up dialog. True when it removed consultations from this computer.</summary>
    Task<bool> RunBackupAsync();

    /// <summary>The Restore dialog. True when it added consultations.</summary>
    Task<bool> RunRestoreAsync();

    /// <summary>
    /// The Add consultation recording dialog, opened on the given file when one was dropped. The
    /// file and its date, or null when cancelled.
    /// </summary>
    Task<RecordingImport?> RunImportAsync(string? path = null);

    /// <summary>The voice enrolment dialog. True when a print was made.</summary>
    Task<bool> RunEnrolmentAsync();

    /// <summary>The reflection sheet for a stored consultation. Closing saves.</summary>
    Task ShowReflectionAsync(string sessionId, string startedAt);
}
