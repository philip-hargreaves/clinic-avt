using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Windows.UI;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;

namespace ClinicAVT.App.Adapters;

// ElementTheme.Default follows Windows live. The shell draws the caption buttons from the app
// window's colours, so they are repainted whenever the effective theme changes
public sealed class WinUiThemeService(WindowAccessor window) : IThemeService
{
    private FrameworkElement? _root;

    public void Apply(AppTheme theme)
    {
        if (window.Window?.Content is not FrameworkElement root)
        {
            return;
        }
        if (!ReferenceEquals(_root, root))
        {
            if (_root is not null)
            {
                _root.ActualThemeChanged -= OnActualThemeChanged;
            }
            _root = root;
            root.ActualThemeChanged += OnActualThemeChanged;
        }
        root.RequestedTheme = theme switch
        {
            AppTheme.Light => ElementTheme.Light,
            AppTheme.Dark => ElementTheme.Dark,
            _ => ElementTheme.Default,
        };
        PaintCaptionButtons(root.ActualTheme);
    }

    private void OnActualThemeChanged(FrameworkElement sender, object args) =>
        PaintCaptionButtons(sender.ActualTheme);

    private void PaintCaptionButtons(ElementTheme theme)
    {
        if (window.Window?.AppWindow.TitleBar is not AppWindowTitleBar bar)
        {
            return;
        }
        var dark = theme == ElementTheme.Dark;
        var glyph = dark ? Colors.White : Color.FromArgb(0xFF, 0x1A, 0x1A, 0x1A);
        bar.ButtonBackgroundColor = Colors.Transparent;
        bar.ButtonInactiveBackgroundColor = Colors.Transparent;
        bar.ButtonForegroundColor = glyph;
        bar.ButtonHoverForegroundColor = glyph;
        bar.ButtonPressedForegroundColor = glyph;
        bar.ButtonInactiveForegroundColor = dark
            ? Color.FromArgb(0xFF, 0x7F, 0x7F, 0x7F)
            : Color.FromArgb(0xFF, 0x9A, 0x9A, 0x9A);
        bar.ButtonHoverBackgroundColor = dark
            ? Color.FromArgb(0x19, 0xFF, 0xFF, 0xFF)
            : Color.FromArgb(0x0F, 0x00, 0x00, 0x00);
        bar.ButtonPressedBackgroundColor = dark
            ? Color.FromArgb(0x0F, 0xFF, 0xFF, 0xFF)
            : Color.FromArgb(0x0A, 0x00, 0x00, 0x00);
    }
}
