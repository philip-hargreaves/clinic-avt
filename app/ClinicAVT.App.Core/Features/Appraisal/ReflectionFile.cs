using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Features.Appraisal;

/// <summary>Saves one reflection as text, showing the identifier warning first.</summary>
public static class ReflectionFile
{
    public static async Task SaveAsync(
        IDialogService dialogs, IFilePicker picker, ITextFiles files, IStatusLine status, string text,
        string title, string warning)
    {
        if (warning.Length > 0 && !await dialogs.ConfirmAsync("Check before saving",
                warning + "\n\nChange the wording, or save as it is.", "Save anyway", "Go back")
            .ConfigureAwait(true))
        {
            return;
        }

        if (await picker.SaveTextAsync(files, FileName(title), "Plain text", ".txt", text).ConfigureAwait(true)
            is { } path)
        {
            status.Append($"Reflection saved to {Path.GetFileName(path)}");
        }
    }

    /// <summary>
    /// "reflection - {title}" with the characters a file name cannot hold blanked.
    /// </summary>
    public static string FileName(string title)
    {
        var invalid = Path.GetInvalidFileNameChars();
        var name = new string(title.Select(c => invalid.Contains(c) ? ' ' : c).ToArray()).Trim();
        return name.Length > 0 ? "reflection - " + name : "reflection";
    }
}
