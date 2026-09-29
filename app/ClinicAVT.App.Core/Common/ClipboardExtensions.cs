using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Common;

public static class ClipboardExtensions
{
    /// <summary>
    /// Copies and reports the outcome on the status line, naming what was copied. Lines end in
    /// CRLF, since an edit box leaves CR alone.
    /// </summary>
    public static async Task CopyAsync(
        this IClipboard clipboard, IStatusLine status, string text, string what)
    {
        status.Append(await clipboard.CopyAsync(text.ReplaceLineEndings("\r\n")).ConfigureAwait(true)
            ? $"{what} copied"
            : "Copy failed - the clipboard is unavailable");
    }
}
