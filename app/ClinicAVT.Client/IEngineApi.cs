namespace ClinicAVT.Client;

/// <summary>
/// The engine as the shell uses it, with one method per request and typed replies.
/// Method names and wire shapes live only in EngineApi.
/// </summary>
public interface IEngineApi
{
    /// <summary>True while a verified transport is up and requests can succeed.</summary>
    bool Connected { get; }

    event Action<bool>? ConnectedChanged;

    event Action<EngineNotification>? NotificationReceived;

    Task<EngineReadiness> ReadinessAsync();

    /// <summary>Asks the engine to leave once this shell disconnects, as soon as no load keeps it.</summary>
    Task RequestExitAsync();

    Task<IReadOnlyList<ModelInfo>> ListModelsAsync();

    Task<EngineMetrics> MetricsAsync(TimeSpan? timeout = null);

    Task<IReadOnlyList<AudioInput>> ListAudioInputsAsync();

    // Every start returns the session id, empty when the engine sent none
    Task<string> StartSessionAsync(bool retain, string micId);

    Task<string> StartReplayAsync(bool retain, ReplayRequest replay);

    Task<string> ResumeSessionAsync(string sessionId, bool retain, ReplayRequest? replay);

    Task<string> StopSessionAsync();

    /// <summary>Reads an audio file's length and recording time without importing it.</summary>
    Task<RecordingInfo> InspectRecordingAsync(string path);

    /// <summary>
    /// Imports an audio file and finalises it as a stop does, completing with the session once
    /// sealed. A cancel throws ImportCancelledException and a lost engine IOException.
    /// </summary>
    Task<string> ImportRecordingAsync(string path, string startedAt, bool retain);

    Task CancelSessionAsync();

    Task PauseSessionAsync(bool paused);

    Task MonitorSessionAsync(bool monitor);

    Task OpenSessionAsync(string id);

    Task CloseSessionAsync();

    Task<IReadOnlyList<SessionSummary>> ListSessionsAsync();

    Task<IReadOnlyList<TranscriptTurn>> TranscriptAsync(string id);

    Task<StoredNote> StoredNoteAsync(string id);

    Task<StoredPatient> StoredPatientAsync(string id);

    /// <summary>The guidance record saved with the note, or null when there is none.</summary>
    Task<GuidanceRecord?> StoredGuidanceAsync(string id);

    Task LabelSessionAsync(string id, string text);

    Task DeleteSessionAsync(string id);

    /// <summary>
    /// Erases every stored consultation and returns how many went. Reflections, with their case
    /// summary, stay unless asked to go too.
    /// </summary>
    Task<int> DeleteAllSessionsAsync(bool deleteReflections = false);

    /// <summary>Erases the given consultations, as after a checked backup, and returns how many went.</summary>
    Task<int> RemoveSessionsAsync(IReadOnlyList<string> ids, bool deleteReflections);

    /// <summary>
    /// Counts what a backup of the half-open UTC period would hold. Empty ends are open. With
    /// the last backup's coverage it also counts what no backup holds.
    /// </summary>
    Task<ArchiveSummary> ArchiveSummaryAsync(
        string periodStart, string periodEnd, ArchiveCoverage? covered = null);

    /// <summary>
    /// Starts a backup. archive/progress, then archive/done or archive/failed follow. Reflections
    /// only writes just the appraisal entries, and backs up no consultation.
    /// </summary>
    Task BackUpAsync(
        string periodStart, string periodEnd, string path, string password, bool reflectionsOnly = false);

    /// <summary>Starts a restore, or on a dry run only reads the file and counts.</summary>
    Task RestoreAsync(string path, string password, bool dryRun);

    Task<NoteTierState> SetNoteTierAsync(string tier);

    /// <summary>Moves speech recognition to "GPU" or "NPU" in place. An asr/device
    /// notification says when it is ready.</summary>
    Task<AsrDeviceState> SetAsrDeviceAsync(string device);

    Task SetNoteOptionsAsync(string style, string detail);

    Task RegenerateNoteAsync(string style, string detail, bool confirmed = false);

    Task UpdateNoteAsync(string id, string text);

    Task RegeneratePatientAsync();

    Task UpdatePatientAsync(string id, string text);

    Task TranslatePatientAsync(string id, string language);

    Task<IReadOnlyList<string>> LanguagesAsync();

    Task<CorporaStatus> GuidanceCorporaAsync();

    Task SearchGuidanceAsync(string id);

    Task SearchGuidanceAsync(string text, int limit);

    Task<GuidancePage> PageAsync(long document, int page, string chunkId);

    Task<DocumentList> ListDocumentsAsync();

    Task<DocumentsAdded> AddDocumentsAsync(IReadOnlyList<string> paths);

    Task RemoveDocumentAsync(long id);

    Task RemoveAllDocumentsAsync();

    /// <summary>The file's path, empty when the engine has none.</summary>
    Task<string> OpenDocumentAsync(long id);

    Task SetResearchGuidanceAsync(bool include);

    Task<AnchorStatus> AnchorStatusAsync();

    Task ClearAnchorAsync();

    Task StartEnrolmentAsync(double seconds, string micId);

    Task CancelEnrolmentAsync();

    Task FinishEnrolmentAsync();

    Task<IReadOnlyList<ReflectionListing>> ListReflectionsAsync();

    Task<StoredReflection> GetReflectionAsync(string id);

    Task SummariseReflectionAsync(string id);

    Task UpdateReflectionAsync(
        string id, string happened, string learned, string nextTime,
        IReadOnlyList<ReflectionReference> references);

    Task UpdateReflectionSummaryAsync(string id, string summary);

    Task DeleteReflectionAsync(string id);

    // Each demo call returns how many consultations were added or removed
    Task<int> SeedDemoAsync();

    Task<int> ClearDemoAsync();
}
