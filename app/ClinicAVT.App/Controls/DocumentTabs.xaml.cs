using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace ClinicAVT.App.Controls;

public sealed partial class DocumentTabs : UserControl
{
    // Items pad their label by 12. The underline overhangs the label by 4 on each side
    private const double LabelPadding = 12;
    private const double Overhang = 4;

    private readonly List<UIElement> _views = [];

    public DocumentTabs()
    {
        InitializeComponent();
        // Tabs move when one shows or hides and when text scales, so the underline follows layout
        Bar.LayoutUpdated += (_, _) => PlaceUnderline();
    }

    /// <summary>The area a view fills, for sizing an editor to it.</summary>
    public FrameworkElement ContentArea => Host;

    public int SelectedIndex
    {
        get => Bar.SelectedItem is { } item ? Bar.Items.IndexOf(item) : -1;
        set => Bar.SelectedItem = Bar.Items[value];
    }

    /// <summary>Adds a view under a heading. The first one added is shown.</summary>
    public void Add(string heading, UIElement view)
    {
        Bar.Items.Add(new SelectorBarItem { Text = heading });
        _views.Add(view);
        view.Visibility = Visibility.Collapsed;
        Host.Children.Add(view);
        if (_views.Count == 1)
        {
            SelectedIndex = 0;
        }
    }

    /// <summary>
    /// Shows or hides a heading. Hiding the selected one selects the first visible heading.
    /// </summary>
    public void SetVisible(int index, bool visible)
    {
        var visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        if (Bar.Items[index].Visibility == visibility)
        {
            return;
        }

        Bar.Items[index].Visibility = visibility;
        if (!visible && SelectedIndex == index)
        {
            for (var i = 0; i < Bar.Items.Count; i++)
            {
                if (Bar.Items[i].Visibility == Visibility.Visible)
                {
                    SelectedIndex = i;
                    return;
                }
            }
        }
    }

    private void OnSelectionChanged(SelectorBar sender, SelectorBarSelectionChangedEventArgs args)
    {
        var chosen = SelectedIndex;
        for (var i = 0; i < _views.Count; i++)
        {
            _views[i].Visibility = i == chosen ? Visibility.Visible : Visibility.Collapsed;
        }

        PlaceUnderline();
    }

    private void PlaceUnderline()
    {
        if (Bar.SelectedItem is not { ActualWidth: > 0 } item)
        {
            Underline.Opacity = 0;
            return;
        }

        var x = item.TransformToVisual(TabRow).TransformPoint(default).X + LabelPadding - Overhang;
        var width = Math.Max(0, item.ActualWidth - 2 * (LabelPadding - Overhang));
        if (UnderlineShift.X != x || Underline.Width != width)
        {
            UnderlineShift.X = x;
            Underline.Width = width;
        }

        Underline.Opacity = 1;
    }
}
