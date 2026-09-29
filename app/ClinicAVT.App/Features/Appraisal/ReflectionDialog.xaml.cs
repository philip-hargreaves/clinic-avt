using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Features.Appraisal;

namespace ClinicAVT.App.Features.Appraisal;

/// <summary>Closing saves changes.</summary>
public sealed partial class ReflectionDialog : ContentDialog
{
    public ReflectionDialog(ReflectionViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        EditorHost.Content = new ReflectionEditorView(viewModel);
    }

    public ReflectionViewModel ViewModel { get; }

    private void OnClosing(ContentDialog sender, ContentDialogClosingEventArgs args)
    {
        // The deferral keeps the dialog until the save has gone to the engine
        var deferral = args.GetDeferral();
        _ = FinishAsync(deferral);
    }

    private async Task FinishAsync(ContentDialogClosingDeferral deferral)
    {
        try
        {
            await ViewModel.CloseAsync();
        }
        finally
        {
            deferral.Complete();
        }
    }
}
