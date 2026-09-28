using System.Text.Json;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.Client;

namespace ClinicAVT.App.Tests.TestDoubles;

/// <summary>
/// Test-double engine. After session/stop or session/import it pushes note/ready then
/// patient/ready, like the real pipeline. An import reports its finalise stages first.
/// </summary>
public sealed class FakeEngineClient(bool autoNotify = true) : IEngineTransport
{
    private static readonly JsonElement Empty = JsonSerializer.SerializeToElement(new { });

    private static readonly string[] FakeLanguages = ["French", "Polish", "Urdu"];

    private bool _importRunning;

    public event Action<string, JsonElement>? NotificationReceived;

    public event Action<bool>? ConnectedChanged;

    public bool Connected { get; private set; } = true;

    public void SetConnected(bool connected)
    {
        Connected = connected;
        ConnectedChanged?.Invoke(connected);
    }

    /// <summary>Every request, as (method, serialised params).</summary>
    public List<(string Method, string Params)> Requests { get; } = [];

    /// <summary>The params of the last request for the method.</summary>
    public JsonElement Sent(string method) =>
        JsonDocument.Parse(Requests.Last(r => r.Method == method).Params).RootElement;

    /// <summary>A scripted reply per method, served before anything below.</summary>
    public Dictionary<string, object> Responses { get; } = [];

    /// <summary>Methods that refuse every time.</summary>
    public HashSet<string> Failing { get; } = [];

    /// <summary>Thrown by the next matching request, once. Null answers normally.</summary>
    public Func<string, Exception?>? FailNext { get; set; }

    /// <summary>
    /// Runs before a request is answered, for notifications the engine pushes
    /// during a blocking request such as finalise stages during session/stop.
    /// </summary>
    public Action<string>? BeforeReply { get; set; }

    public Task<JsonElement> RequestAsync(
        string method, object? parameters, TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        Requests.Add((method, parameters is null ? "" : JsonSerializer.Serialize(parameters, Protocol.JsonOptions)));

        if (FailNext?.Invoke(method) is { } failure)
        {
            FailNext = null;
            return Task.FromException<JsonElement>(failure);
        }

        if (Failing.Contains(method))
        {
            return Task.FromException<JsonElement>(new InvalidOperationException($"{method} refused"));
        }

        BeforeReply?.Invoke(method);
        if (Responses.TryGetValue(method, out var scripted))
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(scripted));
        }

        if (method == "engine/hello")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new PeerInfo(ExpectedEngine.Name, ExpectedEngine.Version, Protocol.ProtocolVersion),
                Protocol.JsonOptions));
        }

        if (method == "session/start")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new { sessionId = "s1" }));
        }

        if (method == "session/stop")
        {
            if (autoNotify)
            {
                _ = NotifySequenceAsync();
            }

            return Task.FromResult(JsonSerializer.SerializeToElement(new { sessionId = "s1" }));
        }

        if (method == "recording/inspect")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new { seconds = RecordingSeconds, recordedAt = "2026-09-26T13:05:00Z" }));
        }

        // The engine answers at once and finalises on its own thread, reporting its stages as
        // session/stop does. Its end can arrive before the answer. HoldImport keeps it running
        if (method == "session/import")
        {
            _importRunning = true;
            if (!HoldImport)
            {
                FinishImport();
            }

            return Task.FromResult(JsonSerializer.SerializeToElement(new { sessionId = "s1" }));
        }

        // A held import stops at its next span and is erased
        if (method == "session/cancel" && _importRunning)
        {
            _importRunning = false;
            RaiseNotification("session/importFailed",
                JsonSerializer.SerializeToElement(new { sessionId = "s1", error = "cancelled" }));
            return Task.FromResult(Empty);
        }

        if (method == "engine/readiness")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new { firstUse = FirstUse, ready = ModelsCompiled, strayNoteHost = StrayNoteHost }));
        }

        if (method == "translate/languages")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new { languages = FakeLanguages }));
        }

        if (method == "audio/inputs")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                devices = AudioInputs
                    .Select(d => new
                    {
                        id = d.Id,
                        name = d.Name,
                        shortName = d.ShortName,
                        isDefault = d.IsDefault,
                        bluetooth = d.Bluetooth,
                    })
                    .ToArray(),
            }));
        }

        if (method == "engine/models")
        {
            var models = new List<object>
            {
                // An ablation export listed first, which the default tier must still beat
                new { id = "whisper-turbo-int8-wordts", name = "", task = "asr", tier = "wordts", device = "GPU", licence = "MIT", active = false },
                new { id = "whisper-turbo-int8", name = "Whisper Large v3 Turbo", task = "asr", tier = "default", device = "GPU", licence = "MIT", active = true },
                new { id = "qwen3.5-9b-int4", name = "Qwen3.5 9B", task = "note", tier = "default", device = "GPU", licence = "Apache-2.0", active = NoteTier == "default" },
            };
            foreach (var (id, name, tier) in ExtraNoteModels)
            {
                models.Add(new { id, name, task = "note", tier, device = "GPU", licence = "Apache-2.0", active = NoteTier == tier });
            }

            return Task.FromResult(JsonSerializer.SerializeToElement(new { models }));
        }

        if (method == "reflection/get")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                id = JsonDocument.Parse(Requests[^1].Params).RootElement.GetProperty("id").GetString(),
                label = "Elbow swelling",
                summary = ReflectionSummary is null ? null : new { text = ReflectionSummary, generatedAt = "2026-09-06T10:00:00Z", editedAt = (string?)null },
                reflection = ReflectionAnswers is null && ReflectionReferences.Count == 0 ? null : new
                {
                    happened = ReflectionAnswers?.Happened ?? "",
                    learned = ReflectionAnswers?.Learned ?? "",
                    next = ReflectionAnswers?.Next ?? "",
                    references = ReflectionReferences.ToArray(),
                    createdAt = "2026-09-06T10:05:00Z",
                    editedAt = (string?)null,
                },
            }));
        }

        if (method == "reflection/summary")
        {
            var id = JsonDocument.Parse(Requests[^1].Params).RootElement.GetProperty("id").GetString();
            if (SummarySilent)
            {
                return Task.FromResult(JsonSerializer.SerializeToElement(new { }));
            }

            if (SummaryFails)
            {
                RaiseNotification("reflection/summaryFailed",
                    JsonSerializer.SerializeToElement(new { id, detail = "the model is not loaded" }));
            }
            else
            {
                RaiseNotification("reflection/summary",
                    JsonSerializer.SerializeToElement(new { id, text = "A patient in their forties presented with a swollen elbow." }));
            }

            return Task.FromResult(JsonSerializer.SerializeToElement(new { }));
        }

        if (method == "reflection/list")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                reflections = Reflections.Select(r => new
                {
                    id = r.Id,
                    startedAt = r.StartedAt,
                    label = r.Label,
                    happened = "",
                    learned = r.Learned,
                    next = "",
                    summary = r.Summary,
                    createdAt = r.StartedAt,
                    editedAt = (string?)null,
                    demo = DemoReflections.Contains(r.Id),
                }).ToArray(),
            }));
        }

        if (method == "demo/seed")
        {
            var added = SamplesSeeded ? 0 : 8;
            SamplesSeeded = true;
            return Task.FromResult(JsonSerializer.SerializeToElement(new { added }));
        }

        if (method == "session/deleteAll")
        {
            var removed = SamplesSeeded ? 8 + StoredSessions : StoredSessions;
            SamplesSeeded = false;
            StoredSessions = 0;
            return Task.FromResult(JsonSerializer.SerializeToElement(new { removed }));
        }

        if (method == "demo/clear")
        {
            var removed = SamplesSeeded ? 8 : 0;
            SamplesSeeded = false;
            return Task.FromResult(JsonSerializer.SerializeToElement(new { removed }));
        }

        if (method == "note/tier")
        {
            var tier = JsonDocument.Parse(Requests[^1].Params).RootElement
                .GetProperty("tier").GetString() ?? "default";
            if (tier == "auto")
            {
                tier = "default";  // the engine's own pick for this machine
            }

            // The loaded tier answers ready, as a warm engine does. A new one starts loading
            var state = tier == NoteTier ? "ready" : "loading";
            NoteTier = tier;
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                tier,
                id = tier == "default" ? "qwen3.5-9b-int4" : "",
                name = tier == "default" ? "Qwen3.5 9B" : "",
                state,
            }));
        }

        if (method == "engine/metrics")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new { devices = new { asr = "GPU.0" }, asrRealtimeFactor = MetricsRealtimeFactor }));
        }

        if (method == "anchor/status")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                origin = AnchorOrigin,
                sessions = AnchorSessions,
                enrolledAt = AnchorEnrolledAt,
            }));
        }

        if (method == "anchor/clear")
        {
            AnchorOrigin = "none";
            AnchorSessions = 0;
            AnchorEnrolledAt = null;
            return Task.FromResult(Empty);
        }

        if (method == "session/transcript")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                turns = Transcript
                    .Select(t => new { firstFrame = 0, frameCount = 0, speaker = t.Speaker, text = t.Text })
                    .ToArray(),
            }));
        }

        if (method == "session/note" && StoredNote is not null)
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                text = StoredNote,
                style = "",
                detail = "",
                generatedAt = "2026-09-13T00:00:00Z",
                editedAt = StoredNoteEditedAt,
            }));
        }

        if (method == "session/patient" && StoredPatient is not null)
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                text = StoredPatient,
                generatedAt = StoredPatientGeneratedAt,
                editedAt = StoredPatientEditedAt,
                translation = StoredTranslation is null ? null : new
                {
                    language = "Polish",
                    text = StoredTranslation,
                    translatedAt = StoredTranslatedAt,
                },
            }));
        }

        if (method == "guidance/corpora")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                state = GuidanceState,
                detail = GuidanceDetail,
                corpora = GuidanceCorpora.ToArray(),
            }));
        }

        if (method == "session/guidance")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new { guidance = StoredGuidance }));
        }

        if (method == "guidance/documents")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                folder = @"C:\Users\clinician\Documents\ClinicAVT guidelines",
                found = GuidelinesFolderFound,
                unsupported = UnsupportedFiles,
                documents = GuidanceDocuments.ToArray(),
            }));
        }

        if (method == "guidance/page" && PageReply is not null)
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(PageReply));
        }

        if (method == "guidance/documents/open")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new { path = OpenedPath }));
        }

        if (method == "guidance/documents/add")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                documents = AddedDocuments.ToArray(),
                skipped = SkippedDocuments.ToArray(),
            }));
        }

        return Task.FromResult(Empty);
    }

    /// <summary>Served by recording/inspect, the shape of the shared fixture.</summary>
    public double RecordingSeconds { get; set; } = 760.4;

    /// <summary>The session/progress stages an import reports before it answers.</summary>
    public List<string> ImportStages { get; } = ["transcript", "speakers", "turns"];

    /// <summary>Leaves an import running until FinishImport or session/cancel.</summary>
    public bool HoldImport { get; set; }

    /// <summary>Seals the import: its stages, session/imported, then the note.</summary>
    public void FinishImport()
    {
        _importRunning = false;
        foreach (var stage in ImportStages)
        {
            RaiseNotification("session/progress", JsonSerializer.SerializeToElement(new { stage }));
        }

        RaiseNotification("session/imported", JsonSerializer.SerializeToElement(new { sessionId = "s1" }));
        if (autoNotify)
        {
            _ = NotifySequenceAsync();
        }
    }

    /// <summary>The import has reached percent of the whole, in stage.</summary>
    public void ImportProgress(string stage, int percent) =>
        RaiseNotification("session/importProgress",
            JsonSerializer.SerializeToElement(new { sessionId = "s1", stage, percent }));

    /// <summary>Served by guidance/documents, empty by default.</summary>
    public List<object> GuidanceDocuments { get; } = [];

    public bool GuidelinesFolderFound { get; set; } = true;

    public int UnsupportedFiles { get; set; }

    /// <summary>Accepted rows and skipped files, served by guidance/documents/add.</summary>
    public List<object> AddedDocuments { get; } = [];

    public List<object> SkippedDocuments { get; } = [];

    /// <summary>Served by guidance/page when set.</summary>
    public object? PageReply { get; set; }

    /// <summary>Served by guidance/documents/open.</summary>
    public string? OpenedPath { get; set; }

    /// <summary>The embedder's state served by guidance/corpora, ready by default.</summary>
    public string GuidanceState { get; set; } = "ready";

    public string? GuidanceDetail { get; set; }

    /// <summary>The corpora guidance/corpora lists, one loaded fixture corpus by default.</summary>
    public List<object> GuidanceCorpora { get; } = [GuidanceRecords.Corpus()];

    /// <summary>Served by session/note when set, otherwise the stored note is empty.</summary>
    public string? StoredNote { get; set; }

    public string? StoredNoteEditedAt { get; set; }

    /// <summary>Served by session/patient when set.</summary>
    public string? StoredPatient { get; set; }

    public string? StoredPatientGeneratedAt { get; set; }

    public string? StoredPatientEditedAt { get; set; }

    /// <summary>The stored sheet's Polish translation, served with it when set.</summary>
    public string? StoredTranslation { get; set; }

    public string? StoredTranslatedAt { get; set; }

    /// <summary>Served by session/guidance, null until a record is stored.</summary>
    public JsonElement? StoredGuidance { get; set; }

    /// <summary>Turns served by session/transcript after a stop.</summary>
    public List<(string Speaker, string Text)> Transcript { get; } = [];

    /// <summary>Served by engine/metrics, healthy by default.</summary>
    public double MetricsRealtimeFactor { get; set; } = 33.4;

    public List<ClinicAVT.App.Core.Features.Consultation.MicDevice> AudioInputs { get; set; } = [];

    /// <summary>Served by anchor/status, nothing learned by default.</summary>
    public string AnchorOrigin { get; set; } = "none";

    public int AnchorSessions { get; set; }

    public long? AnchorEnrolledAt { get; set; }

    /// <summary>Note models beyond the 9B that engine/models lists, as (id, name, tier).</summary>
    public List<(string Id, string Name, string Tier)> ExtraNoteModels { get; } = [];

    /// <summary>The tier the engine's note lane is on. note/tier moves it.</summary>
    public string NoteTier { get; set; } = "default";

    /// <summary>Served by engine/readiness, warm and compiled by default.</summary>
    public bool FirstUse { get; set; }

    public bool ModelsCompiled { get; set; } = true;

    /// <summary>Served by reflection/get, null until a summary was written.</summary>
    public string? ReflectionSummary { get; set; }

    /// <summary>Served by reflection/get, null until the clinician wrote something.</summary>
    public (string Happened, string Learned, string Next)? ReflectionAnswers { get; set; }

    /// <summary>The guidance ticked as referred to, served by reflection/get.</summary>
    public List<ReflectionReference> ReflectionReferences { get; } = [];

    public bool SummaryFails { get; set; }

    /// <summary>The request is taken and nothing ever comes back, as when the engine dies writing it.</summary>
    public bool SummarySilent { get; set; }

    /// <summary>Served by reflection/list.</summary>
    public List<(string Id, string StartedAt, string Label, string Learned, string Summary)> Reflections { get; } = [];

    /// <summary>Which of Reflections are seeded samples.</summary>
    public HashSet<string> DemoReflections { get; } = [];

    /// <summary>Whether demo/seed has run. demo/clear resets it.</summary>
    public bool SamplesSeeded { get; set; }

    /// <summary>Real consultations in the store, counted by session/deleteAll.</summary>
    public int StoredSessions { get; set; }

    /// <summary>Served by engine/readiness, no wedged note process by default.</summary>
    public bool StrayNoteHost { get; set; }

    public void RaiseNotification(string method, JsonElement parameters = default) =>
        NotificationReceived?.Invoke(method, parameters);

    private async Task NotifySequenceAsync()
    {
        await Task.Delay(700).ConfigureAwait(false);
        RaiseNotification("note/ready");
        await Task.Delay(1500).ConfigureAwait(false);
        RaiseNotification("patient/ready");
    }

    public ValueTask DisposeAsync() => ValueTask.CompletedTask;
}
