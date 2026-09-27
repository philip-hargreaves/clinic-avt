using System.Text.Json;

namespace ClinicAVT.Client;

/// <summary>IEngineApi over a transport. The wire names and shapes live here.</summary>
public sealed class EngineApi : IEngineApi
{
    private readonly IEngineTransport _transport;

    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);

    // For store and folder work such as a model switch, a batch of documents or an erase
    private static readonly TimeSpan LongTimeout = TimeSpan.FromSeconds(30);

    // Longer than the engine's 10 s no-audio deadline. A Bluetooth link can take
    // seconds to wake, and a timeout here would abandon a started session
    private static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(30);

    // A post-crash resume decrypts stored audio first
    private static readonly TimeSpan ResumeTimeout = TimeSpan.FromSeconds(60);

    // A stop runs the whole finalise, from transcript through speakers to note
    private static readonly TimeSpan StopTimeout = TimeSpan.FromSeconds(180);

    public EngineApi(IEngineTransport transport)
    {
        _transport = transport;
        transport.NotificationReceived += OnNotification;
    }

    public event Action<EngineNotification>? NotificationReceived;

    public bool Connected => _transport.Connected;

    public event Action<bool>? ConnectedChanged
    {
        add => _transport.ConnectedChanged += value;
        remove => _transport.ConnectedChanged -= value;
    }

    public Task<EngineReadiness> ReadinessAsync() => ReplyAsync<EngineReadiness>("engine/readiness");

    public Task RequestExitAsync() => CallAsync("engine/exit");

    public Task<IReadOnlyList<ModelInfo>> ListModelsAsync() =>
        ListAsync<ModelInfo>("engine/models", "models");

    public async Task<EngineMetrics> MetricsAsync(TimeSpan? timeout = null)
    {
        var reply = await CallAsync("engine/metrics", null, timeout).ConfigureAwait(false);
        return Parse<EngineMetrics>("engine/metrics", reply) with { Raw = reply };
    }

    public Task<IReadOnlyList<AudioInput>> ListAudioInputsAsync() =>
        ListAsync<AudioInput>("audio/inputs", "devices");

    // The engine pins the microphone. An empty id means the default
    public Task<string> StartSessionAsync(bool retain, string micId) =>
        StartAsync(new { retain, micId }, StartTimeout);

    public Task<string> StartReplayAsync(bool retain, ReplayRequest replay) =>
        StartAsync(new { retain, replay }, StartTimeout);

    public Task<string> StartPlaybackAsync(string sessionId) =>
        StartAsync(new { playback = new { id = sessionId } }, StartTimeout);

    public Task<string> ResumeSessionAsync(string sessionId, bool retain, ReplayRequest? replay) =>
        StartAsync(
            replay is null
                ? new { resume = sessionId, retain }
                : new { resume = sessionId, retain, replay },
            ResumeTimeout);

    public async Task<string> StopSessionAsync() =>
        Text(await CallAsync("session/stop", null, StopTimeout).ConfigureAwait(false), "sessionId");

    public Task CancelSessionAsync() => CallAsync("session/cancel");

    public Task PauseSessionAsync(bool paused) => CallAsync("session/pause", new { paused });

    public Task MonitorSessionAsync(bool monitor) => CallAsync("session/monitor", new { on = monitor });

    public Task OpenSessionAsync(string id) => CallAsync("session/open", new { id });

    public Task CloseSessionAsync() => CallAsync("session/close");

    public Task<IReadOnlyList<SessionSummary>> ListSessionsAsync() =>
        ListAsync<SessionSummary>("session/list", "sessions");

    public Task<IReadOnlyList<TranscriptTurn>> TranscriptAsync(string id) =>
        ListAsync<TranscriptTurn>("session/transcript", "turns", new { id });

    public Task<StoredNote> StoredNoteAsync(string id) => ReplyAsync<StoredNote>("session/note", new { id });

    public Task<StoredPatient> StoredPatientAsync(string id) =>
        ReplyAsync<StoredPatient>("session/patient", new { id });

    public async Task<GuidanceRecord?> StoredGuidanceAsync(string id)
    {
        var reply = await CallAsync("session/guidance", new { id }).ConfigureAwait(false);
        return reply.TryProperty("guidance", JsonValueKind.Object, out var record)
            ? Protocol.Parse<GuidanceRecord>(record)
            : null;
    }

    public Task LabelSessionAsync(string id, string text) => CallAsync("session/label", new { id, text });

    public Task DeleteSessionAsync(string id) => CallAsync("session/delete", new { id });

    public async Task<int> DeleteAllSessionsAsync() =>
        Int(await CallAsync("session/deleteAll", null, LongTimeout).ConfigureAwait(false), "removed");

    public Task<NoteTierState> SetNoteTierAsync(string tier) =>
        ReplyAsync<NoteTierState>("note/tier", new { tier }, LongTimeout);

    public Task<AsrDeviceState> SetAsrDeviceAsync(string device) =>
        ReplyAsync<AsrDeviceState>("asr/device", new { device });

    public Task SetNoteOptionsAsync(string style, string detail) =>
        CallAsync("note/options", new { style, detail });

    public Task RegenerateNoteAsync(string style, string detail, bool confirmed = false) =>
        CallAsync("note/regenerate",
            confirmed ? new { style, detail, confirmed } : new { style, detail });

    public Task UpdateNoteAsync(string id, string text) => CallAsync("note/update", new { id, text });

    public Task RegeneratePatientAsync() => CallAsync("patient/regenerate");

    public Task UpdatePatientAsync(string id, string text) =>
        CallAsync("patient/update", new { id, text });

    public Task TranslatePatientAsync(string id, string language) =>
        CallAsync("patient/translate", new { id, language });

    public Task<IReadOnlyList<string>> LanguagesAsync() =>
        ListAsync<string>("translate/languages", "languages");

    public Task<CorporaStatus> GuidanceCorporaAsync() => ReplyAsync<CorporaStatus>("guidance/corpora");

    public Task SearchGuidanceAsync(string id) => CallAsync("guidance/search", new { id });

    public Task SearchGuidanceAsync(string text, int limit) =>
        CallAsync("guidance/search", new { text, limit });

    public Task<GuidancePage> PageAsync(long document, int page, string chunkId) =>
        ReplyAsync<GuidancePage>("guidance/page", new { id = document, page, chunkId });

    public Task<DocumentList> ListDocumentsAsync() => ReplyAsync<DocumentList>("guidance/documents");

    public Task<DocumentsAdded> AddDocumentsAsync(IReadOnlyList<string> paths) =>
        ReplyAsync<DocumentsAdded>("guidance/documents/add", new { paths }, LongTimeout);

    public Task RemoveDocumentAsync(long id) => CallAsync("guidance/documents/remove", new { id }, LongTimeout);

    public Task RemoveAllDocumentsAsync() => CallAsync("guidance/documents/removeAll", null, LongTimeout);

    public async Task<string> OpenDocumentAsync(long id) =>
        Text(await CallAsync("guidance/documents/open", new { id }).ConfigureAwait(false), "path");

    public Task SetResearchGuidanceAsync(bool include) =>
        CallAsync("guidance/research", new { include }, LongTimeout);

    public Task<AnchorStatus> AnchorStatusAsync() => ReplyAsync<AnchorStatus>("anchor/status");

    public Task ClearAnchorAsync() => CallAsync("anchor/clear");

    public Task StartEnrolmentAsync(double seconds, string micId) =>
        CallAsync("anchor/enrol", new { seconds, mic = new { id = micId } });

    public Task CancelEnrolmentAsync() => CallAsync("anchor/enrol/cancel");

    public Task FinishEnrolmentAsync() => CallAsync("anchor/enrol/finish");

    public Task<IReadOnlyList<ReflectionListing>> ListReflectionsAsync() =>
        ListAsync<ReflectionListing>("reflection/list", "reflections");

    public Task<StoredReflection> GetReflectionAsync(string id) =>
        ReplyAsync<StoredReflection>("reflection/get", new { id });

    public Task SummariseReflectionAsync(string id) => CallAsync("reflection/summary", new { id });

    public Task UpdateReflectionAsync(
        string id, string happened, string learned, string nextTime,
        IReadOnlyList<ReflectionReference> references) =>
        CallAsync("reflection/update", new { id, happened, learned, next = nextTime, references });

    public Task UpdateReflectionSummaryAsync(string id, string summary) =>
        CallAsync("reflection/update", new { id, summary });

    public Task DeleteReflectionAsync(string id) => CallAsync("reflection/delete", new { id });

    public async Task<int> SeedDemoAsync() =>
        Int(await CallAsync("demo/seed", null, LongTimeout).ConfigureAwait(false), "added");

    public async Task<int> ClearDemoAsync() =>
        Int(await CallAsync("demo/clear", null, LongTimeout).ConfigureAwait(false), "removed");

    private async Task<string> StartAsync(object parameters, TimeSpan timeout) =>
        Text(await CallAsync("session/start", parameters, timeout).ConfigureAwait(false), "sessionId");

    private Task<JsonElement> CallAsync(string method, object? parameters = null, TimeSpan? timeout = null) =>
        _transport.RequestAsync(method, parameters, timeout ?? Timeout);

    private async Task<T> ReplyAsync<T>(string method, object? parameters = null, TimeSpan? timeout = null)
        where T : class =>
        Parse<T>(method, await CallAsync(method, parameters, timeout).ConfigureAwait(false));

    private async Task<IReadOnlyList<T>> ListAsync<T>(string method, string property, object? parameters = null)
    {
        var reply = await CallAsync(method, parameters).ConfigureAwait(false);
        if (!reply.TryProperty(property, JsonValueKind.Array, out var list))
        {
            throw new InvalidOperationException($"{method}: no {property} in the reply");
        }

        return list.Deserialize<List<T>>(Protocol.JsonOptions)!;
    }

    private static T Parse<T>(string method, JsonElement reply)
        where T : class =>
        Protocol.Parse<T>(reply) ?? throw new InvalidOperationException($"{method}: malformed reply");

    // A notification the shell does not know, or cannot read, is dropped here
    private void OnNotification(string method, JsonElement parameters)
    {
        if (EngineNotifications.Parse(method, parameters) is { } notification)
        {
            NotificationReceived?.Invoke(notification);
        }
    }

    private static string Text(JsonElement reply, string property) => reply.Text(property) ?? "";

    private static int Int(JsonElement reply, string property) =>
        reply.TryProperty(property, JsonValueKind.Number, out var value) ? value.GetInt32() : 0;
}
