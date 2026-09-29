using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Backup;

namespace ClinicAVT.App.Features.Backup;

/// <summary>
/// Open reads the file and shows what it holds. Restore then adds what is not here.
/// </summary>
public sealed partial class RestoreDialog : ContentDialog
{
    public RestoreDialog(RestoreViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    public RestoreViewModel ViewModel { get; }

    private void OnPrimary(ContentDialog sender, ContentDialogButtonClickEventArgs args)
    {
        args.Cancel = true;
        UiEvent.Run(() => ViewModel.PrimaryCommand.ExecuteAsync(null));
    }

    private void OnClosing(ContentDialog sender, ContentDialogClosingEventArgs args) =>
        args.Cancel = ViewModel.BlocksClose;

    private void OnPasswordChanged(object sender, RoutedEventArgs e) =>
        ViewModel.Password = ((PasswordBox)sender).Password;
}
