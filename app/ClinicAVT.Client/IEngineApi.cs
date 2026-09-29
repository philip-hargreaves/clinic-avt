namespace ClinicAVT.Client;

/// <summary>Typed engine requests. Wire names and shapes live only in EngineApi.</summary>
public interface IEngineApi
    : IEngineControl, IRecordingApi, ISessionStoreApi, INoteApi, IGuidanceApi, IEnrolmentApi,
        IReflectionApi, IArchiveApi
{
}

/// <summary>Connection state and pushed notifications, common to every role interface.</summary>
public interface IEngineLink
{
    /// <summary>True while a verified transport is up and requests can succeed.</summary>
    bool Connected { get; }

    event Action<bool>? ConnectedChanged;

    event Action<EngineNotification>? NotificationReceived;
}

public interface IEngineControl : IEngineLink
{
    Task<EngineReadiness> ReadinessAsync();

    /// <summary>Asks the engine to exit when idle after this shell disconnects.</summary>
    Task RequestExitAsync();

    Task<IReadOnlyList<ModelInfo>> ListModelsAsync();

    Task<EngineMetrics> MetricsAsync(TimeSpan? timeout = null);

    Task<NoteTierState> SetNoteTierAsync(string tier);

    /// <summary>Moves speech recognition to "GPU" or "NPU" in place. An asr/device
    /// notification says when it is ready.</summary>
    Task<AsrDeviceState> SetAsrDeviceAsync(AsrDevice device);
}

/// <summary>Live recording and file import.</summary>
public interface IRecordingApi : IEngineLink
{
    Task<IReadOnlyList<AudioInput>> ListAudioInputsAsync();

    // Every start returns the session id, empty when the engine sent none
    Task<string> StartSessionAsync(bool retain, string micId);

    Task<string> ResumeSessionAsync(string sessionId, bool retain);

    Task<string> StopSessionAsync();

    Task CancelSessionAsync();

    /// <summary>Reads an audio file's length and recording time without importing it.</summary>
    Task<RecordingInfo> InspectRecordingAsync(string path);

    /// <summary>
    /// Imports an audio file and finalises it as a stop does, completing with the session once
    /// sealed. A cancel throws ImportCancelledException and a lost engine IOException.
    /// </summary>
    Task<string> ImportRecordingAsync(string path, string startedAt, bool retain);
}

public interface ISessionStoreApi : IEngineLink
{
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
    /// Deletes all consultations and returns the count. Reflections are kept unless
    /// deleteReflections is set.
    /// </summary>
    Task<int> DeleteAllSessionsAsync(bool deleteReflections = false);

    // Both return how many sample consultations were added or removed
    Task<int> SeedSamplesAsync();

    Task<int> ClearSamplesAsync();
}

/// <summary>Note and patient sheet generation, edits and translation.</summary>
public interface INoteApi : IEngineLink
{
    Task SetNoteOptionsAsync(string style, string detail);

    Task RegenerateNoteAsync(string style, string detail, bool confirmed = false);

    Task UpdateNoteAsync(string id, string text);

    Task RegeneratePatientAsync();

    Task UpdatePatientAsync(string id, string text);

    Task TranslatePatientAsync(string id, string language);

    Task<IReadOnlyList<string>> LanguagesAsync();
}

public interface IGuidanceApi : IEngineLink
{
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
}

public interface IEnrolmentApi : IEngineLink
{
    Task<AnchorStatus> AnchorStatusAsync();

    Task ClearAnchorAsync();

    Task StartEnrolmentAsync(double seconds, string micId);

    Task CancelEnrolmentAsync();

    Task FinishEnrolmentAsync();
}

public interface IReflectionApi : IEngineLink
{
    Task<IReadOnlyList<ReflectionListing>> ListReflectionsAsync();

    Task<StoredReflection> GetReflectionAsync(string id);

    Task SummariseReflectionAsync(string id);

    Task UpdateReflectionAsync(
        string id, string happened, string learned, string nextTime,
        IReadOnlyList<ReflectionReference> references);

    Task UpdateReflectionSummaryAsync(string id, string summary);

    Task DeleteReflectionAsync(string id);
}

/// <summary>Encrypted backup and restore.</summary>
public interface IArchiveApi : IEngineLink
{
    /// <summary>
    /// Counts what a backup of the half-open UTC period would hold. Empty ends are open. With
    /// the last backup's coverage it also counts what no backup holds.
    /// </summary>
    Task<ArchiveSummary> ArchiveSummaryAsync(
        string periodStart, string periodEnd, ArchiveCoverage? covered = null);

    /// <summary>
    /// Starts a backup. Progress arrives as archive/progress, then archive/done or archive/failed.
    /// reflectionsOnly writes appraisal entries only.
    /// </summary>
    Task BackUpAsync(
        string periodStart, string periodEnd, string path, string password, bool reflectionsOnly = false);

    /// <summary>Starts a restore, or on a dry run only reads the file and counts.</summary>
    Task RestoreAsync(string path, string password, bool dryRun);

    /// <summary>
    /// Deletes the given consultations (e.g. after a verified backup) and returns the count.
    /// </summary>
    Task<int> RemoveSessionsAsync(IReadOnlyList<string> ids, bool deleteReflections);
}
