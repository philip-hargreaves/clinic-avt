using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Common;
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

    public static Visibility Shown(string text) =>
        string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;

    private void OnPrimary(ContentDialog sender, ContentDialogButtonClickEventArgs args)
    {
        args.Cancel = true;
        UiEvent.Run(() => ViewModel.PrimaryCommand.ExecuteAsync(null));
    }

    // Covers the close button, Escape and a click outside
    private void OnClosing(ContentDialog sender, ContentDialogClosingEventArgs args) =>
        args.Cancel = ViewModel.BlocksClose;

    // PasswordBox.Password is not bindable
    private void OnPasswordChanged(object sender, RoutedEventArgs e) =>
        ViewModel.Password = ((PasswordBox)sender).Password;

    private void OnAgainChanged(object sender, RoutedEventArgs e) =>
        ViewModel.PasswordAgain = ((PasswordBox)sender).Password;
}
