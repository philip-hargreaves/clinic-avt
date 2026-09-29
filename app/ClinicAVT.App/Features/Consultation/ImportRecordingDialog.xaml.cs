using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Features.Consultation;

namespace ClinicAVT.App.Features.Consultation;

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
