using Microsoft.UI.Xaml;

namespace ClinicAVT.App.Adapters;

/// <summary>The control that opened a pane, so focus can go back to it when the pane closes.</summary>
public sealed class FocusReturn
{
    public FrameworkElement? Opener { get; set; }

    public void Return()
    {
        Opener?.Focus(FocusState.Programmatic);
        Opener = null;
    }
}
