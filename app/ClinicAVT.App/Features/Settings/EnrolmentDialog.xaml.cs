using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Settings;

namespace ClinicAVT.App.Features.Settings;

/// <summary>
/// The primary button steps through Start, Finish, then Done or Try again. Cancel stops a
/// reading and keeps nothing.
/// </summary>
public sealed partial class EnrolmentDialog : ContentDialog
{
    public EnrolmentDialog(EnrolmentViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    public EnrolmentViewModel ViewModel { get; }

    // Only Done closes. Every other press keeps the dialog open for the next step
    private void OnPrimary(ContentDialog sender, ContentDialogButtonClickEventArgs args)
    {
        args.Cancel = ViewModel.KeepsOpen;
        UiEvent.Run(() => ViewModel.PrimaryCommand.ExecuteAsync(null));
    }

    // Closing covers the close button, Escape and a click outside
    private void OnClosing(ContentDialog sender, ContentDialogClosingEventArgs args) => ViewModel.Dismiss();
}
