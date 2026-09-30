using System.Text.RegularExpressions;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Common;

public static partial class EngineWords
{
    /// <summary>
    /// The engine's refusal message, reworded for clinicians. Any other failure gets a plain
    /// message and its detail is logged.
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

        // Windows reports most causes as a call and an error code, which go to the log. The
        // privacy setting is the only cause a clinician can act on
        if (e is EngineErrorException { Code: Protocol.CaptureFailedCode })
        {
            return e.Message.StartsWith("microphone access denied", StringComparison.OrdinalIgnoreCase)
                ? "microphone access is off in Windows Settings, under Privacy & security, Microphone"
                : Reworded("the microphone could not be opened", e, logger);
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
