using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using ClinicAVT.App.Core.Shell;

namespace ClinicAVT.App.Controls;

/// <summary>The partner marks as a row of images, in the theme's variant of each.</summary>
internal static class PartnerMarks
{
    // Also rebuilt on Loaded. A view that is off the tree when the theme switches gets no
    // ActualThemeChanged
    public static void Attach(FrameworkElement view, Panel row, IReadOnlyList<CreditMark> marks)
    {
        void Build() => Fill(row, marks, view.ActualTheme == ElementTheme.Dark);
        Build();
        view.ActualThemeChanged += (_, _) => Build();
        view.Loaded += (_, _) => Build();
    }

    private static void Fill(Panel row, IReadOnlyList<CreditMark> marks, bool dark)
    {
        row.Children.Clear();
        foreach (var mark in marks)
        {
            var path = dark ? mark.DarkPath : mark.LightPath;
            var uri = new Uri(path);
            row.Children.Add(new Image
            {
                Height = mark.Height,
                Margin = new Thickness(mark.Space, 0, 0, 0),
                Source = path.EndsWith(".svg", StringComparison.OrdinalIgnoreCase)
                    ? new SvgImageSource(uri)
                    : new BitmapImage(uri),
                VerticalAlignment = VerticalAlignment.Center,
            });
        }
    }
}
