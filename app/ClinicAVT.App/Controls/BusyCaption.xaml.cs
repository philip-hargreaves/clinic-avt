using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace ClinicAVT.App.Controls;

/// <summary>A small progress ring beside a caption. Each part hides when it has nothing to show.</summary>
public sealed partial class BusyCaption : UserControl
{
    public static readonly DependencyProperty TextProperty = DependencyProperty.Register(
        nameof(Text), typeof(string), typeof(BusyCaption),
        new PropertyMetadata("", (d, e) => ((BusyCaption)d).ShowText((string)e.NewValue)));

    public static readonly DependencyProperty IsActiveProperty = DependencyProperty.Register(
        nameof(IsActive), typeof(bool), typeof(BusyCaption), new PropertyMetadata(false));

    public static readonly DependencyProperty TextStyleProperty = DependencyProperty.Register(
        nameof(TextStyle), typeof(Style), typeof(BusyCaption),
        new PropertyMetadata(null, (d, e) => ((BusyCaption)d).Restyle((Style?)e.NewValue)));

    public static readonly DependencyProperty TextWrappingProperty = DependencyProperty.Register(
        nameof(TextWrapping), typeof(TextWrapping), typeof(BusyCaption),
        new PropertyMetadata(TextWrapping.NoWrap));

    public BusyCaption() => InitializeComponent();

    public string Text
    {
        get => (string)GetValue(TextProperty);
        set => SetValue(TextProperty, value);
    }

    public bool IsActive
    {
        get => (bool)GetValue(IsActiveProperty);
        set => SetValue(IsActiveProperty, value);
    }

    /// <summary>One line unless set. A long caption in a narrow pane needs Wrap.</summary>
    public TextWrapping TextWrapping
    {
        get => (TextWrapping)GetValue(TextWrappingProperty);
        set => SetValue(TextWrappingProperty, value);
    }

    /// <summary>The caption's text style. MicroText unless set.</summary>
    public Style? TextStyle
    {
        get => (Style?)GetValue(TextStyleProperty);
        set => SetValue(TextStyleProperty, value);
    }

    private void ShowText(string text) =>
        Caption.Visibility = text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;

    private void Restyle(Style? style)
    {
        if (style is not null)
        {
            Caption.Style = style;
        }
    }
}
