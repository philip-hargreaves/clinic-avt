using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;
using ClinicAVT.App.Controls;
using ClinicAVT.App.Core.Shell;

namespace ClinicAVT.App.Shell;

public sealed partial class StatusBarView : UserControl
{
    private static readonly Size Unbounded = new(double.PositiveInfinity, double.PositiveInfinity);

    public StatusBarView(StatusBarViewModel viewModel, CreditsViewModel credits)
    {
        ViewModel = viewModel;
        InitializeComponent();
        PartnerMarks.Attach(this, CreditsRow, credits.Marks);
        Loaded += (_, _) => FitChips();
        // Chip text changes width, so refit after the bindings update
        ViewModel.Chips.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(ModelChips.AsrChip)
                or nameof(ModelChips.NoteChip)
                or nameof(ModelChips.MemoryChip)
                or nameof(ModelChips.MetricsVisible))
            {
                DispatcherQueue.TryEnqueue(FitChips);
            }
        };
        ViewModel.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(StatusBarViewModel.DisplayLabel))
            {
                DispatcherQueue.TryEnqueue(FitChips);
            }
        };
    }

    public StatusBarViewModel ViewModel { get; }

    private void OnBarSizeChanged(object sender, SizeChangedEventArgs e) => FitChips();

    // Chips show only if all fit between the state line and partner marks, so none is clipped
    private void FitChips()
    {
        if (Bar.ActualWidth <= 0)
        {
            return;
        }

        Chips.Visibility = Visibility.Visible;
        Chips.Measure(Unbounded);
        State.Measure(Unbounded);
        CreditsRow.Measure(Unbounded);
        var room = Bar.ActualWidth - State.DesiredSize.Width - CreditsRow.DesiredSize.Width - 3 * Bar.ColumnSpacing;
        Chips.Visibility = Chips.DesiredSize.Width <= room ? Visibility.Visible : Visibility.Collapsed;
    }
}
