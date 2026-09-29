namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>The consultation under review and what the store holds for it.</summary>
public sealed class ReviewedSession
{
    /// <summary>The session the documents belong to, null before a stop or an open.</summary>
    public string? FinalisedSessionId { get; set; }

    /// <summary>When the stored session began, empty for the one just recorded.</summary>
    public string StartedAt { get; set; } = "";

    /// <summary>
    /// True while the review shows a stored session. False for the one just recorded.
    /// </summary>
    public bool StoredOpen { get; set; }

    /// <summary>What the store holds, for autosaving in-place edits on leave.</summary>
    public string LoadedNote { get; set; } = "";

    public string LoadedPatient { get; set; } = "";

    /// <summary>
    /// True from a regenerate request until its pipeline settles. It keeps the rewrite out of
    /// the per-session metrics.
    /// </summary>
    public bool Regenerating { get; set; }

    // The stored note while an example case stands in for it, so it can come back
    public string? OriginalNote { get; set; }
}
