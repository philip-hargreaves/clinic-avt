using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Appraisal;

namespace ClinicAVT.App.Features.Appraisal;

/// <summary>
/// The primary button advances the view model and never closes the dialog, as in BackupDialog.
/// </summary>
public sealed partial class ExportReflectionsDialog : ContentDialog
{
    public ExportReflectionsDialog(ExportReflectionsViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    public ExportReflectionsViewModel ViewModel { get; }

    private void OnPrimary(ContentDialog sender, ContentDialogButtonClickEventArgs args)
    {
        args.Cancel = true;
        UiEvent.Run(() => ViewModel.PrimaryCommand.ExecuteAsync(null));
    }

    // Covers the close button, Escape and a click outside
    private void OnClosing(ContentDialog sender, ContentDialogClosingEventArgs args) =>
        args.Cancel = ViewModel.BlocksClose;
}
