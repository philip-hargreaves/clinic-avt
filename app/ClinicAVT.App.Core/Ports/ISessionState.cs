namespace ClinicAVT.App.Core.Ports;

public interface ISessionState
{
    /// <summary>True while a consultation is recorded or finalised, until its note is written.</summary>
    bool ConsultationInProgress { get; }

    /// <summary>The consultation whose review is on screen, or null.</summary>
    string? ReviewedSessionId { get; }

    /// <summary>Closes the review on screen, saving edits. Nothing when none is open.</summary>
    Task EndReviewAsync();

    /// <summary>Where the session was, for the crash log. Empty when unknown.</summary>
    string SessionPhase { get; }
}
