using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using ClinicAVT.App.Themes;

namespace ClinicAVT.App.Controls;

/// <summary>
/// An accent border marks a box in edit mode, and the box takes focus. Otherwise it keeps the
/// flat document look.
/// </summary>
internal static class EditingChrome
{
    public static void Show(TextBox box, bool editing)
    {
        if (editing)
        {
            box.BorderBrush = ThemedResources.GetBrush("AccentFillColorDefaultBrush", box.ActualTheme);
            box.BorderThickness = new Thickness(1.5);
            // The box shows once its binding has run, so focus waits for the next dispatch
            box.DispatcherQueue.TryEnqueue(() => box.Focus(FocusState.Programmatic));
        }
        else
        {
            box.ClearValue(Control.BorderBrushProperty);
            box.ClearValue(Control.BorderThicknessProperty);
        }
    }
}
