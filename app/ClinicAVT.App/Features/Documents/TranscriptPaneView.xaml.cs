using System.Collections.Specialized;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Core.Features.Documents;

namespace ClinicAVT.App.Features.Documents;

public sealed partial class TranscriptPaneView : UserControl
{
    public TranscriptPaneView(TranscriptViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        viewModel.Turns.CollectionChanged += OnTurnsChanged;
    }

    public TranscriptViewModel ViewModel { get; }

    /// <summary>
    /// Caps the turn list so it ends level with the note column and scrolls beyond that.
    /// Infinity lifts the cap when the pane has the area to itself.
    /// </summary>
    public void CapHeight(double maxHeight)
    {
        var capped = !double.IsPositiveInfinity(maxHeight);
        TurnList.MaxHeight = capped ? Math.Max(160, maxHeight) : double.PositiveInfinity;
        TurnList.VerticalAlignment = capped ? VerticalAlignment.Top : VerticalAlignment.Stretch;
    }

    // Keeps the newest turn in view
    private void OnTurnsChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (ViewModel.Turns.Count > 0)
        {
            TurnList.ScrollIntoView(ViewModel.Turns[^1]);
        }
    }
}
