namespace ClinicAVT.App.Core.Ports;

public interface ISessionState
{
    /// <summary>
    /// True while a consultation is being recorded or finalised, until its note is written. A
    /// finished consultation on screen for review is not in progress.
    /// </summary>
    bool ConsultationInProgress { get; }

    /// <summary>The consultation whose review is on screen, or null.</summary>
    string? ReviewedSessionId => null;

    /// <summary>Closes the review on screen, saving edits. Nothing when none is open.</summary>
    Task EndReviewAsync() => Task.CompletedTask;

    /// <summary>Where the session was, for the crash log. Empty when unknown.</summary>
    string SessionPhase => "";
}
