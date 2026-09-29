using System.ComponentModel;
using ClinicAVT.App.Core.Features.Consultation;

namespace ClinicAVT.App.Core.Ports;

/// <summary>The consultation on the Consultation page, and the stored one opened for review.</summary>
public interface IConsultation : ISessionState, INotifyPropertyChanged
{
    SessionState State { get; }

    FinalisePhase Phase { get; }

    double AudioSeconds { get; }

    bool Importing { get; }

    string? ImportLine { get; }

    /// <summary>False while the engine is still starting or reconnecting.</summary>
    bool EngineReady { get; }

    bool ModelsReady { get; }

    /// <summary>
    /// True while the review shows a stored consultation. False for the one just recorded.
    /// </summary>
    bool ReviewingStored { get; }

    /// <summary>
    /// The id of the consultation just recorded while its review is up. Null once a stored one
    /// replaces it.
    /// </summary>
    string? LiveReviewId { get; }

    /// <summary>The session the engine is recording into.</summary>
    string? RecordingSessionId { get; }

    /// <summary>Raised when a stop or import seals a session.</summary>
    event Action<string>? Sealed;

    /// <summary>An import began, with when the consultation took place.</summary>
    event Action<RecordingImport>? ImportStarted;

    Task StartRecordingAsync();

    Task StopRecordingAsync();

    Task CancelRecordingAsync();

    /// <summary>
    /// The Add consultation recording dialog, on a dropped file when there is one. An open
    /// review ends before the import starts.
    /// </summary>
    Task ImportRecordingAsync(string? path = null);

    Task CancelImportAsync();

    Task<bool> OpenStoredSessionAsync(string id, string startedLabel = "",
        string startedAt = "", bool hasReflection = false, bool sample = false);

    Task CloseReviewAsync();

    void FinishConsultation();
}
