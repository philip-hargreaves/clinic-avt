using ClinicAVT.App.Core.Ports;
using Windows.ApplicationModel.DataTransfer;

namespace ClinicAVT.App.Platform;

public sealed class WinUiClipboard : IClipboard
{
    // A DataPackage can be handed to the clipboard only once, so a retry needs a fresh one
    public async Task<bool> CopyAsync(string text)
    {
        for (var attempt = 1; attempt <= 5; attempt++)
        {
            try
            {
                var data = new DataPackage();
                data.SetText(text);
                Clipboard.SetContent(data);
                // A Flush refusal must not fail a SetContent that succeeded
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
}
