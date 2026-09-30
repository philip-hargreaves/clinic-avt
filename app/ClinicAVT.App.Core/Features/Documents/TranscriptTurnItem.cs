namespace ClinicAVT.App.Core.Features.Documents;

/// <summary>One transcript row. Live turns carry no speaker until the seal.</summary>
public sealed record TranscriptTurnItem(string Speaker, string TimeLabel, string Text)
{
    /// <summary>
    /// "Doctor", "Patient", "Other speaker" for anyone else, an ellipsis while unknown, or the
    /// engine's fallback label capitalised ("Speaker 1") when roles could not be decided.
    /// </summary>
    public string SpeakerLabel => Speaker switch
    {
        "doctor" => "Doctor",
        "patient" => "Patient",
        "unknown" => "Other speaker",
        "" => "…",
        _ => char.ToUpperInvariant(Speaker[0]) + Speaker[1..],
    };
}
