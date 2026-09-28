using System.Text.RegularExpressions;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Common;

public static class ModelNames
{
    /// <summary>The manifest's name, or one made from the id when it has none.</summary>
    public static string Display(ModelInfo model) =>
        string.IsNullOrWhiteSpace(model.Name) ? Friendly(model.Id) : model.Name;

    /// <summary>
    /// The display name for a manifest that has none. "whisper-turbo-int8" reads as
    /// "Whisper Turbo". Precision tokens are dropped and size tokens such as "9b" kept.
    /// </summary>
    public static string Friendly(string id)
    {
        var words = id.Split('-')
            .Where(t => t is not ("int8" or "int4" or "fp16" or "fp32"))
            .Select(t => Regex.IsMatch(t, @"^\d+b$")
                ? t.ToUpperInvariant()
                : char.ToUpperInvariant(t[0]) + t[1..]);
        return string.Join(' ', words);
    }

    /// <summary>
    /// Names the missing roles a consultation needs, such as "the speech recognition model is not
    /// installed". Matches the wording of the engine's refusals.
    /// </summary>
    public static string Missing(IReadOnlyList<string> roles)
    {
        var names = roles
            .Select(role => role switch
            {
                "asr" => "speech recognition",
                "vad" => "speech detection",
                "diarisation" or "segmentation" => "speaker recognition",
                _ => role,
            })
            .Distinct()
            .ToList();
        return names.Count == 1
            ? $"the {names[0]} model is not installed"
            : $"the {string.Join(", ", names[..^1])} and {names[^1]} models are not installed";
    }
}
