using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Features.Consultation;

namespace ClinicAVT.App.Features.Consultation;

/// <summary>Add closes the dialog and hands the file over.</summary>
public sealed partial class ImportRecordingDialog : ContentDialog
{
    public ImportRecordingDialog(ImportRecordingViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        AudioDrop.Attach(DropArea, DropHighlight, () => true, ViewModel.UseFileAsync);
    }

    public ImportRecordingViewModel ViewModel { get; }
}
