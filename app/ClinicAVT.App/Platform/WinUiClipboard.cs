using ClinicAVT.App.Core.Ports;
using Windows.ApplicationModel.DataTransfer;

namespace ClinicAVT.App.Platform;

public sealed class WinUiClipboard : IClipboard
{
    // A Flush refusal must not fail a SetContent that succeeded
    public Task<bool> CopyAsync(string text) => SetWithRetryAsync(text, data =>
    {
        Clipboard.SetContent(data);
        try
        {
            Clipboard.Flush();
        }
        catch (Exception)
        {
        }

        return true;
    });

    // No Flush, so the secret leaves the clipboard when the app closes
    public Task<bool> CopySecretAsync(string text) => SetWithRetryAsync(text, data =>
        Clipboard.SetContentWithOptions(
            data, new ClipboardContentOptions { IsAllowedInHistory = false, IsRoamable = false }));

    // A DataPackage can be handed to the clipboard only once, so a retry needs a fresh one
    private static async Task<bool> SetWithRetryAsync(string text, Func<DataPackage, bool> set)
    {
        for (var attempt = 1; attempt <= 5; attempt++)
        {
            try
            {
                var data = new DataPackage();
                data.SetText(text);
                return set(data);
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
