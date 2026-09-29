using System.Text.Json;
using System.Text.Json.Serialization;

namespace ClinicAVT.Client;

// Engine reply records. Defaults let sparse replies parse; a null string means the field
// was absent

public sealed record EngineReadiness(bool FirstUse = false, bool Ready = true, bool StrayNoteHost = false)
{
    /// <summary>
    /// The roles a consultation needs that are not installed, such as "asr". Consultations are
    /// refused while any is missing. An older engine sends none.
    /// </summary>
    public IReadOnlyList<string> Missing { get; init; } = [];
}

public sealed record ModelInfo(
    string Id = "", string? Name = null, ModelTask Task = ModelTask.Other, string Tier = "",
    string Device = "", bool Active = false);

public sealed record EngineDevices(string? Asr = null, string? Note = null);

public sealed record EngineMetrics(double? AsrRealtimeFactor = null, EngineDevices? Devices = null)
{
    /// <summary>The whole snapshot, for the metrics log.</summary>
    [JsonIgnore]
    public JsonElement Raw { get; init; }
}

public sealed record AudioInput(
    string Id = "", string? Name = null, string? ShortName = null, bool IsDefault = false,
    bool Bluetooth = false);

/// <summary>
/// An audio file's length, and when it was recorded as an ISO UTC instant: the container's
/// own time, else the file's modified time.
/// </summary>
public sealed record RecordingInfo(double Seconds = 0, string? RecordedAt = null);

public sealed record SessionSummary(
    string Id = "", string StartedAt = "", string EndedAt = "", string? Label = null,
    string? EditedAt = null, double AudioSeconds = 0,
    [property: JsonPropertyName("demo")] bool Sample = false, bool HasReflection = false);

public sealed record TranscriptTurn(
    string Speaker = "", ulong FirstFrame = 0, string Text = "");

public sealed record StoredNote(
    string? Text = null, string? Style = null, string? Detail = null, string? GeneratedAt = null,
    string? EditedAt = null);

public sealed record StoredTranslation(
    string? Text = null, string? Language = null, string? TranslatedAt = null);

public sealed record StoredPatient(
    string? Text = null, string? GeneratedAt = null, string? EditedAt = null,
    StoredTranslation? Translation = null);

public sealed record NoteTierState(
    string Tier = "", string Id = "", string Name = "", ModelState State = ModelState.Unknown);

public sealed record CorpusInfo(
    string Id = "", string Name = "", string? Attribution = null, string? Source = null,
    int Chunks = 0, string? BuiltAt = null, string? Unavailable = null);

public sealed record CorporaStatus(CorporaState State = CorporaState.Unknown, string? Detail = null)
{
    public IReadOnlyList<CorpusInfo> Corpora { get; init; } = [];
}

public sealed record DocumentInfo(
    long Id = 0, string? Name = null, string? Path = null, DocumentState State = DocumentState.Unknown,
    DocumentError? Error = null, int Pages = 0, int PagesWithoutText = 0, int Chunks = 0,
    string? AddedAt = null);

public sealed record DocumentList(string? Folder = null, bool Found = true, int Unsupported = 0)
{
    public IReadOnlyList<DocumentInfo> Documents { get; init; } = [];
}

public sealed record SkippedFile(string? Path = null, string? Reason = null);

public sealed record DocumentsAdded
{
    public IReadOnlyList<DocumentInfo> Documents { get; init; } = [];

    public IReadOnlyList<SkippedFile> Skipped { get; init; } = [];
}

/// <summary>One box of the cited passage, as fractions of the page.</summary>
public sealed record PassageBox(
    int Page = 0, double Left = 0, double Top = 0, double Right = 0, double Bottom = 0);

public sealed record GuidancePage(
    string? Path = null, double Width = 0, double Height = 0, int Pages = 0)
{
    public IReadOnlyList<PassageBox> Boxes { get; init; } = [];
}

/// <summary>One recommendation of a guidance search, as the wire gives it.</summary>
public sealed record GuidanceResult(
    string? Corpus = null, string? ChunkId = null, string? Code = null, string? Number = null,
    string? Title = null, string? Section = null, string? Text = null, string? Url = null,
    string? LastUpdated = null, string? UpdateTag = null, string? Source = null,
    string? Citation = null, string? Trigger = null, long Document = 0, int Page = 0, int Pages = 0);

/// <summary>A corpus the search covered, labelled when its manifest names a publisher.</summary>
public sealed record SearchedCorpus(string Id = "", string Name = "", string? Label = null);

/// <summary>
/// A guidance search. Id is set for the note's search and null for a typed query. The same
/// record is stored with a note.
/// </summary>
public sealed record GuidanceRecord(
    string? Id = null, string? Detail = null, string? StoreError = null, bool? Stale = false,
    bool DocumentsChanged = false)
{
    public IReadOnlyList<GuidanceResult> Shown { get; init; } = [];

    public IReadOnlyList<SearchedCorpus> Searched { get; init; } = [];
}

public sealed record AnchorStatus(
    AnchorOrigin Origin = AnchorOrigin.None, int Sessions = 0, long? EnrolledAt = null);

public sealed record ReflectionSummary(
    string? Text = null, string? GeneratedAt = null, string? EditedAt = null);

/// <summary>A ticked guideline or document, stored with a copy of its text.</summary>
public sealed record ReflectionReference(
    string Key = "", string Reference = "", string Title = "", string Link = "",
    string Source = "");

public sealed record ReflectionAnswers(
    string? Happened = null, string? Learned = null, string? Next = null,
    string? EditedAt = null)
{
    public IReadOnlyList<ReflectionReference> References { get; init; } = [];
}

public sealed record StoredReflection(
    string Id = "", string? Label = null, ReflectionSummary? Summary = null,
    [property: JsonPropertyName("reflection")] ReflectionAnswers? Answers = null);

public sealed record ReflectionListing(
    string Id = "", string StartedAt = "", string? Label = null, string? Happened = null,
    string? Learned = null, string? Next = null, string? Summary = null, string? EditedAt = null,
    [property: JsonPropertyName("demo")] bool Sample = false);

/// <summary>
/// What a backup of a period would hold. Unfinished consultations are counted and left out.
/// Uncovered counts the stored consultations the given last backup does not hold.
/// </summary>
public sealed record ArchiveSummary(
    int Consultations = 0, int Reflections = 0, int Unfinished = 0, int Uncovered = 0);

/// <summary>The period a checked backup held and when it was made, as UTC instants.</summary>
public sealed record ArchiveCoverage(string From, string To, string At);
