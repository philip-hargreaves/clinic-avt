using System.Diagnostics;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Adapters;

public sealed class WinUiLauncher : ILauncher
{
    // Web links only. Anything else is refused before it reaches the launcher
    public async Task<bool> OpenLinkAsync(string link)
    {
        try
        {
            return WebLinks.IsWeb(link)
                && await Windows.System.Launcher.LaunchUriAsync(new Uri(link));
        }
        catch (Exception)
        {
            return false;
        }
    }

    public async Task OpenFileAsync(string path)
    {
        var file = await Windows.Storage.StorageFile.GetFileFromPathAsync(path);
        if (!await Windows.System.Launcher.LaunchFileAsync(file))
        {
            throw new InvalidOperationException("no PDF viewer answered");
        }
    }

    public void RevealFolder(string path)
    {
        try
        {
            Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
        }
        catch (Exception)
        {
            // The caller's caption already says where the folder is
        }
    }
}
