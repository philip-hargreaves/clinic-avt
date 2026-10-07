namespace ClinicAVT.App.Core.Features.Documents;

/// <summary>
/// Generation is sequential, so the pipeline has exactly these states. A
/// patient leaflet without a clinical note cannot be represented.
/// </summary>
public enum NotePipelineState
{
    Pending,
    NoteWriting,
    NoteReadyPatientWriting,
    AllReady,
    NoteFailed,
    PatientFailed,
    NoteRefused,
}

/// <summary>Engine-reported progress.</summary>
public enum NotePipelineEvent
{
    NoteWritingStarted,
    NoteReady,
    NoteFailed,
    NoteRefused,
    PatientInfoReady,
    PatientInfoFailed,
}
