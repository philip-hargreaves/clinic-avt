using System.Text.Json;

namespace ClinicAVT.Client;

public abstract record EngineNotification;

/// <summary>A streamed lane carries the model's token rate.</summary>
public interface IMetered
{
    double? TokensPerSecond { get; }
}

public sealed record AudioLevel(double Level = 0, bool Clipped = false)
    : EngineNotification;

public sealed record SessionInterrupted(string? Reason = null, string? Detail = null)
    : EngineNotification;

public sealed record SessionProgress(FinaliseStage Stage = FinaliseStage.Unknown) : EngineNotification;

/// <summary>
/// Import progress. Stage is reading, speech, transcribing or finalising. Percent spans all stages.
/// </summary>
public sealed record ImportProgress(
    string SessionId = "", ImportStage Stage = ImportStage.Unknown, int Percent = 0)
    : EngineNotification;

/// <summary>Import stored. The note is generated next, as after a stop.</summary>
public sealed record ImportDone(string SessionId = "") : EngineNotification;

/// <summary>An import failed and nothing was kept. Error is the engine's reason or
/// "cancelled".</summary>
public sealed record ImportFailed(string SessionId = "", string Error = "") : EngineNotification;

public sealed record EnrolmentProgress(
    double Level = 0, double Elapsed = 0, double Speech = 0, bool Clipped = false)
    : EngineNotification;

public sealed record EnrolmentDone(bool Ok = false, string? Detail = null) : EngineNotification;

/// <summary>
/// Speech recognition moving to another device. It reports loading, then ready or failed.
/// </summary>
public sealed record AsrDeviceState(
    AsrDevice Device = AsrDevice.Unknown, ModelState State = ModelState.Unknown, string? Detail = null)
    : EngineNotification;

public sealed record NoteModelState(
    ModelState State = ModelState.Unknown, string Tier = "", string Id = "", string? Name = null, bool FirstUse = false,
    double? Seconds = null, string? Detail = null)
    : EngineNotification;

public sealed record NotePartial(string Text = "", double? TokensPerSecond = null)
    : EngineNotification, IMetered;

public sealed record NoteReady(string? Text = null, double? TokensPerSecond = null)
    : EngineNotification, IMetered;

public sealed record NoteRefused(string Reason = "", bool Overridable = true) : EngineNotification;

public sealed record NoteFailed(string Detail = "failed") : EngineNotification;

public sealed record PatientPartial(string Text = "", double? TokensPerSecond = null)
    : EngineNotification, IMetered;

public sealed record PatientReady(string? Text = null, double? TokensPerSecond = null)
    : EngineNotification, IMetered;

public sealed record PatientFailed(string Detail = "failed") : EngineNotification;

public sealed record TranslationPartial(string Text = "", double? TokensPerSecond = null)
    : EngineNotification, IMetered;

public sealed record TranslationReady(
    string Text = "", string Language = "", double? TokensPerSecond = null)
    : EngineNotification, IMetered;

public sealed record TranslationFailed(string? Detail = null) : EngineNotification;

public sealed record GuidanceModelChanged(CorporaState State = CorporaState.Unknown, string? Detail = null)
    : EngineNotification;

/// <summary>
/// A finished search. The record has an id for the note's search and none for a typed query.
/// </summary>
public sealed record GuidanceReady(GuidanceRecord Record) : EngineNotification;

public sealed record GuidanceFailed(string? Id = null, string? Detail = null) : EngineNotification;

public sealed record GuidanceDocumentsChanged : EngineNotification;

public sealed record GuidanceDocumentChanged(DocumentInfo Document) : EngineNotification;

public sealed record GuidanceProgress(
    long Id = 0, IngestPhase Phase = IngestPhase.Unknown, int Done = 0, int Total = 0)
    : EngineNotification;

public sealed record ReflectionSummaryReady(string Id = "", string Text = "") : EngineNotification;

public sealed record ReflectionSummaryFailed(string Id = "", string Detail = "")
    : EngineNotification;

/// <summary>
/// The store stopped taking writes, such as on a full disk. Sent once per failure.
/// </summary>
public sealed record StorageFault(string Detail = "") : EngineNotification;

/// <summary>A backup or restore under way. Job is "backup" or "restore".</summary>
public sealed record ArchiveProgress(
    ArchiveJob Job = ArchiveJob.Unknown, ArchivePhase Phase = ArchivePhase.Unknown, int Done = 0,
    int Total = 0)
    : EngineNotification;

/// <summary>
/// A backup or restore finished. For a backup, Ids were written and verified. For a restore, the
/// counts are what was added, or would be on a dry run, and Skipped were already present.
/// ReflectionsOnly means the file holds only appraisal entries.
/// </summary>
public sealed record ArchiveDone(
    ArchiveJob Job = ArchiveJob.Unknown, bool DryRun = false, int Consultations = 0,
    int Reflections = 0, int Skipped = 0, string? From = null, string? To = null,
    string? CreatedAt = null, bool ReflectionsOnly = false)
    : EngineNotification
{
    public IReadOnlyList<string> Ids { get; init; } = [];
}

/// <summary>
/// A backup or restore that stopped. Code is one of a fixed set, never file content.
/// </summary>
public sealed record ArchiveFailed(
    ArchiveJob Job = ArchiveJob.Unknown, ArchiveError Code = ArchiveError.Unknown)
    : EngineNotification;

public static class EngineNotifications
{
    /// <summary>
    /// Parses a notification, returning null for an unknown method or an unreadable payload. An
    /// event whose fields are all optional defaults to an empty record.
    /// </summary>
    public static EngineNotification? Parse(string method, JsonElement parameters) => method switch
    {
        "audio.level" => Protocol.Parse<AudioLevel>(parameters),
        "session/interrupted" => Protocol.Parse<SessionInterrupted>(parameters) ?? new SessionInterrupted(),
        "session/progress" => Protocol.Parse<SessionProgress>(parameters),
        "session/importProgress" => Protocol.Parse<ImportProgress>(parameters),
        "session/imported" => Protocol.Parse<ImportDone>(parameters),
        "session/importFailed" => Protocol.Parse<ImportFailed>(parameters),
        "anchor/progress" => Protocol.Parse<EnrolmentProgress>(parameters),
        "anchor/enrolled" => Protocol.Parse<EnrolmentDone>(parameters),
        "note/model" => Protocol.Parse<NoteModelState>(parameters),
        "asr/device" => Protocol.Parse<AsrDeviceState>(parameters),
        "note/partial" => Protocol.Parse<NotePartial>(parameters),
        "note/ready" => Protocol.Parse<NoteReady>(parameters) ?? new NoteReady(),
        "note/refused" => Protocol.Parse<NoteRefused>(parameters) ?? new NoteRefused(),
        "note/failed" => Protocol.Parse<NoteFailed>(parameters) ?? new NoteFailed(),
        "patient/partial" => Protocol.Parse<PatientPartial>(parameters),
        "patient/ready" => Protocol.Parse<PatientReady>(parameters) ?? new PatientReady(),
        "patient/failed" => Protocol.Parse<PatientFailed>(parameters) ?? new PatientFailed(),
        "translate/partial" => Protocol.Parse<TranslationPartial>(parameters),
        "translate/ready" => Protocol.Parse<TranslationReady>(parameters),
        "translate/failed" => Protocol.Parse<TranslationFailed>(parameters) ?? new TranslationFailed(),
        "guidance/model" => Protocol.Parse<GuidanceModelChanged>(parameters) ?? new GuidanceModelChanged(),
        "guidance/ready" => Protocol.Parse<GuidanceRecord>(parameters) is { } record
            ? new GuidanceReady(record)
            : null,
        "guidance/failed" => Protocol.Parse<GuidanceFailed>(parameters),
        "guidance/documentsChanged" => new GuidanceDocumentsChanged(),
        "guidance/document" => Protocol.Parse<DocumentInfo>(parameters) is { } document
            ? new GuidanceDocumentChanged(document)
            : null,
        "guidance/progress" => Protocol.Parse<GuidanceProgress>(parameters),
        "reflection/summary" => Protocol.Parse<ReflectionSummaryReady>(parameters),
        "reflection/summaryFailed" => Protocol.Parse<ReflectionSummaryFailed>(parameters),
        "storage/fault" => Protocol.Parse<StorageFault>(parameters) ?? new StorageFault(),
        "archive/progress" => Protocol.Parse<ArchiveProgress>(parameters),
        "archive/done" => Protocol.Parse<ArchiveDone>(parameters),
        "archive/failed" => Protocol.Parse<ArchiveFailed>(parameters) ?? new ArchiveFailed(),
        _ => null,
    };
}
