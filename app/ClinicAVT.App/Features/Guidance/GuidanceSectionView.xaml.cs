using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media.Animation;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Adapters;

namespace ClinicAVT.App.Features.Guidance;

public sealed partial class GuidanceSectionView : UserControl
{
    private readonly FocusReturn _focus;
    private readonly DispatcherQueueTimer _timingTimer;
    private Storyboard? _fade;

    public GuidanceSectionView(
        GuidanceViewModel viewModel, GuidanceSearchViewModel search, ShellViewModel shell, FocusReturn focus)
    {
        ViewModel = viewModel;
        Search = search;
        Shell = shell;
        _focus = focus;
        InitializeComponent();
        _timingTimer = DispatcherQueue.CreateTimer();
        _timingTimer.Interval = TimeSpan.FromSeconds(4);
        _timingTimer.IsRepeating = false;
        _timingTimer.Tick += (_, _) => FadeTiming();
        ViewModel.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(GuidanceViewModel.FoundIn) && ViewModel.FoundIn.Length > 0)
            {
                _fade?.Stop();
                Timing.Opacity = 1;
                _timingTimer.Start();
            }
        };
    }

    public GuidanceViewModel ViewModel { get; }

    public GuidanceSearchViewModel Search { get; }

    public ShellViewModel Shell { get; }

    private void OnQuerySubmitted(AutoSuggestBox sender, AutoSuggestBoxQuerySubmittedEventArgs e)
    {
        if (Search.SearchQueryCommand.CanExecute(null))
        {
            Search.SearchQueryCommand.Execute(null);
        }
    }

    private void OnQueryKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != Windows.System.VirtualKey.Escape)
        {
            return;
        }

        if (Search.ClearQueryCommand.CanExecute(null))
        {
            Search.ClearQueryCommand.Execute(null);
        }

        Search.Query = "";
        e.Handled = true;
    }

    private void FadeTiming()
    {
        var fade = new DoubleAnimation { To = 0, Duration = TimeSpan.FromMilliseconds(400) };
        Storyboard.SetTarget(fade, Timing);
        Storyboard.SetTargetProperty(fade, "Opacity");
        _fade = new Storyboard();
        _fade.Children.Add(fade);
        _fade.Begin();
    }

    private void OnRowEntered(object sender, PointerRoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is GuidanceRecommendation found)
        {
            ViewModel.Hovered = found.Trigger;
        }
    }

    private void OnRowExited(object sender, PointerRoutedEventArgs e) => ViewModel.Hovered = "";

    /// <summary>Null for an empty tip, so no blank tooltip shows.</summary>
    public static object? Tip(string tip) => tip.Length == 0 ? null : tip;

    // The opener is remembered so focus can return to it when the page closes
    private void OnShowInDocument(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is GuidanceRecommendation found)
        {
            _focus.Opener = sender as FrameworkElement;
            ViewModel.ShowInDocumentCommand.Execute(found);
        }
    }
}
