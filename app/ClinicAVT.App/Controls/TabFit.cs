using Microsoft.UI.Xaml;

namespace ClinicAVT.App.Controls;

/// <summary>Caps a document at a share of its tabbed area's height.</summary>
internal sealed class TabFit(FrameworkElement document, double share)
{
    public void Fit(FrameworkElement area)
    {
        if (area.ActualHeight > 0)
        {
            document.MaxHeight = Math.Max(document.MinHeight, area.ActualHeight * share);
        }
    }

    /// <summary>
    /// Caps the document at the area's height less the chrome the editor stacks around it.
    /// </summary>
    public void FitWithin(FrameworkElement area, double chrome)
    {
        if (area.ActualHeight > 0)
        {
            document.MaxHeight = Math.Max(document.MinHeight, area.ActualHeight - chrome);
        }
    }
}
