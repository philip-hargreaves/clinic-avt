using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace ClinicAVT.App.Controls;

/// <summary>A dialog's warning, announced as it appears and collapsed while Text is empty.</summary>
public sealed partial class WarningLine : UserControl
{
    public static readonly DependencyProperty TextProperty = DependencyProperty.Register(
        nameof(Text), typeof(string), typeof(WarningLine), new PropertyMetadata("", OnTextChanged));

    public WarningLine() => InitializeComponent();

    public string Text
    {
        get => (string)GetValue(TextProperty);
        set => SetValue(TextProperty, value);
    }

    private static void OnTextChanged(DependencyObject d, DependencyPropertyChangedEventArgs e) =>
        ((WarningLine)d).Visibility = string.IsNullOrEmpty(e.NewValue as string)
            ? Visibility.Collapsed
            : Visibility.Visible;
}
