using System.Text.RegularExpressions;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Common;

public static partial class EngineWords
{
    /// <summary>
    /// The engine's reason for a refusal, its internal nouns swapped for the clinician's. Any other
    /// failure reads as plain words, and its detail goes to the log instead.
    /// </summary>
    public static string Reason(Exception e, ILogger? logger = null)
    {
        if (e is OperationCanceledException)
        {
            return Reworded("ClinicAVT didn't respond in time", e, logger);
        }

        if (e is not EngineErrorException)
        {
            return Reworded("something went wrong", e, logger);
        }

        // A store lookup names the missing id, which means nothing to a clinician
        var text = NoSuchSession().Replace(e.Message, "that consultation is no longer on this computer");
        text = Sessions().Replace(text, "consultations");
        text = Session().Replace(text, "consultation");
        return Archive().Replace(text, "backup");
    }

    private static string Reworded(string shown, Exception e, ILogger? logger)
    {
        logger?.Reworded(shown, $"{e.GetType().Name}: {e.Message}");
        return shown;
    }

    [GeneratedRegex(@"\bno (such )?session\b.*$", RegexOptions.IgnoreCase)]
    private static partial Regex NoSuchSession();

    [GeneratedRegex(@"\bsessions\b")]
    private static partial Regex Sessions();

    [GeneratedRegex(@"\bsession\b")]
    private static partial Regex Session();

    [GeneratedRegex(@"\barchive\b")]
    private static partial Regex Archive();
}
