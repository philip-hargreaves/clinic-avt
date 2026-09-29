namespace ClinicAVT.App.Core.Ports;

/// <summary>
/// The dialogs the shell shows on a view model's behalf. Confirmations default
/// to cancel.
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

    /// <summary>Shows the dialog for the view model until it closes. True when its primary button closed it.</summary>
    Task<bool> ShowAsync(object viewModel);
}
