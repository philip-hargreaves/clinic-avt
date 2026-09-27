using System.Diagnostics;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.UI.WindowsAndMessaging;

namespace ClinicAVT.App.Platform;

/// <summary>
/// One app per sign-in. A second launch would find the engine's pipe taken and sit on
/// "Not running", so it brings the open window forward and leaves instead.
/// </summary>
public static class SingleInstance
{
    private static Mutex? _claim;

    /// <summary>True for the first instance, which then holds the claim until it exits.</summary>
    public static bool Claim()
    {
        _claim = new Mutex(initiallyOwned: true, @"Local\ClinicAVT.App", out var first);
        return first;
    }

    /// <summary>Shows the window of the instance already running.</summary>
    public static void ShowOther()
    {
        using var self = Process.GetCurrentProcess();
        foreach (var other in Process.GetProcessesByName(self.ProcessName))
        {
            using (other)
            {
                if (other.Id == self.Id || other.MainWindowHandle == IntPtr.Zero)
                {
                    continue;
                }

                var window = new HWND(other.MainWindowHandle);
                PInvoke.ShowWindow(window, SHOW_WINDOW_CMD.SW_RESTORE);
                PInvoke.SetForegroundWindow(window);
            }
        }
    }
}
