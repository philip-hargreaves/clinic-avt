using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Common;

public static class FilePickerExtensions
{
    /// <summary>
    /// Asks where to save and writes the text there. The path, or null when cancelled.
    /// </summary>
    public static async Task<string?> SaveTextAsync(
        this IFilePicker picker, ITextFiles files, string suggestedName, string typeLabel, string extension,
        string text)
    {
        var path = await picker.PickSaveAsync(suggestedName, typeLabel, extension).ConfigureAwait(true);
        if (path is null)
        {
            return null;
        }

        await files.WriteAsync(path, text).ConfigureAwait(true);
        return path;
    }
}
