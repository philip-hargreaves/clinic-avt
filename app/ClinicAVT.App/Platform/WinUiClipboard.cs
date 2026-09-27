using ClinicAVT.App.Core.Ports;
using Windows.ApplicationModel.DataTransfer;

namespace ClinicAVT.App.Platform;

public sealed class WinUiClipboard : IClipboard
{
    // A DataPackage can be handed to SetContent only once, so a retry needs a
    // fresh one. A Flush refusal must not fail a SetContent that succeeded
    public async Task<bool> CopyAsync(string text)
    {
        for (var attempt = 1; attempt <= 5; attempt++)
        {
            try
            {
                var data = new DataPackage();
                data.SetText(text);
                Clipboard.SetContent(data);
                try
                {
                    Clipboard.Flush();
                }
                catch (Exception)
                {
                }

                return true;
            }
            catch (Exception)
            {
                if (attempt == 5)
                {
                    return false;
                }

                await Task.Delay(80 * attempt);
            }
        }

        return false;
    }

    // No Flush, so the secret leaves the clipboard when the app closes
    public async Task<bool> CopySecretAsync(string text)
    {
        var options = new ClipboardContentOptions { IsAllowedInHistory = false, IsRoamable = false };
        for (var attempt = 1; attempt <= 5; attempt++)
        {
            try
            {
                var data = new DataPackage();
                data.SetText(text);
                return Clipboard.SetContentWithOptions(data, options);
            }
            catch (Exception)
            {
                if (attempt == 5)
                {
                    return false;
                }

                await Task.Delay(80 * attempt);
            }
        }

        return false;
    }
}
