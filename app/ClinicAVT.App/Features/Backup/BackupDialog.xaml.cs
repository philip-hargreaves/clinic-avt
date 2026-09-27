using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Features.Backup;

namespace ClinicAVT.App.Features.Backup;

/// <summary>
/// The primary button steps the view model on and never closes the dialog itself. The dialog
/// stays open while the engine writes, since the job cannot be stopped part way.
/// </summary>
public sealed partial class BackupDialog : ContentDialog
{
    public BackupDialog(BackupViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    public BackupViewModel ViewModel { get; }

    /// <summary>A line that shows only when it has something to say.</summary>
    public static Visibility Shown(string text) =>
        string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;

    private async void OnPrimary(ContentDialog sender, ContentDialogButtonClickEventArgs args)
    {
        args.Cancel = true;
        await ViewModel.PrimaryCommand.ExecuteAsync(null);
    }

    // Covers the close button, Escape and a click outside
    private void OnClosing(ContentDialog sender, ContentDialogClosingEventArgs args) =>
        args.Cancel = ViewModel.BlocksClose;

    // A PasswordBox has no binding for its text, so each change is handed over here
    private void OnOwnChanged(object sender, RoutedEventArgs e) =>
        ViewModel.OwnPassword = ((PasswordBox)sender).Password;

    private void OnAgainChanged(object sender, RoutedEventArgs e) =>
        ViewModel.OwnAgain = ((PasswordBox)sender).Password;
}
