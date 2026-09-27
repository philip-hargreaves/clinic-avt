using System.Text.RegularExpressions;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Common;

/// <summary>The engine's reason for a refusal, in the words the rest of the app uses.</summary>
public static partial class EngineWords
{
    /// <summary>
    /// The engine's reason with its internal nouns swapped for the clinician's. Anything that is
    /// not an engine refusal keeps its own message.
    /// </summary>
    public static string Reason(Exception e)
    {
        if (e is OperationCanceledException)
        {
            return "the engine did not answer in time";
        }

        if (e is not EngineErrorException)
        {
            return e.Message;
        }

        // A store lookup names the missing id, which means nothing to a clinician
        var text = NoSuchSession().Replace(e.Message, "that consultation is no longer on this computer");
        text = Sessions().Replace(text, "consultations");
        text = Session().Replace(text, "consultation");
        return Archive().Replace(text, "backup");
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
