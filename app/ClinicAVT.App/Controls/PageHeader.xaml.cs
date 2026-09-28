using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace ClinicAVT.App.Controls;

public sealed partial class PageHeader : UserControl
{
    public static readonly DependencyProperty TitleProperty = DependencyProperty.Register(
        nameof(Title), typeof(string), typeof(PageHeader), new PropertyMetadata(""));

    public static readonly DependencyProperty CommandsProperty = DependencyProperty.Register(
        nameof(Commands), typeof(UIElement), typeof(PageHeader), new PropertyMetadata(null));

    public PageHeader() => InitializeComponent();

    public string Title
    {
        get => (string)GetValue(TitleProperty);
        set => SetValue(TitleProperty, value);
    }

    public UIElement? Commands
    {
        get => (UIElement?)GetValue(CommandsProperty);
        set => SetValue(CommandsProperty, value);
    }
}
