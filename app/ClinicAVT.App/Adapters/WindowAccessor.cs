using Microsoft.UI.Xaml;

namespace ClinicAVT.App.Adapters;

/// <summary>The main window, once launched, for adapters that need its handle or XAML root.</summary>
public sealed class WindowAccessor
{
    public Window? Window { get; set; }

    public XamlRoot? XamlRoot => (Window?.Content as FrameworkElement)?.XamlRoot;

    /// <summary>An unpackaged app must bind each picker to the window handle.</summary>
    public bool BindToWindow(object picker)
    {
        if (Window is null)
        {
            return false;
        }

        WinRT.Interop.InitializeWithWindow.Initialize(
            picker, WinRT.Interop.WindowNative.GetWindowHandle(Window));
        return true;
    }
}
