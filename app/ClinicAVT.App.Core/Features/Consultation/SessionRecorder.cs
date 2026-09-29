using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Metrics;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// Live session state machine from Idle through Recording and Finalising, with start, stop
/// and resume after an engine restart. Review and NotificationRouter drive the transitions
/// declared here.
/// </summary>
public sealed partial class SessionRecorder : ObservableObject
{
    private readonly IRecordingApi _engine;
    private readonly ISessionStoreApi _store;
    private readonly IStatusLine _status;
    private readonly ConsultationActivity _activity;
    private readonly NoteViewModel _note;
    private readonly GuidanceViewModel _guidance;
    private readonly ReviewPanes _panes;
    private readonly TranscriptViewModel _transcript;
    private readonly PerformanceCollector _metrics;
    private readonly AppPreferences _preferences;

    public SessionRecorder(
        IRecordingApi engine, ISessionStoreApi store, IStatusLine status, ConsultationActivity activity,
        NoteViewModel note, GuidanceViewModel guidance, ReviewPanes panes, TranscriptViewModel transcript,
        PerformanceCollector metrics, AppPreferences preferences)
    {
        _engine = engine;
        _store = store;
        _status = status;
        _activity = activity;
        _note = note;
        _guidance = guidance;
        _panes = panes;
        _transcript = transcript;
        _metrics = metrics;
        _preferences = preferences;
    }

    [ObservableProperty]
    public partial SessionState State { get; private set; } = SessionState.Idle;

    /// <summary>Progress of a stop. Only moves forward within one stop.</summary>
    [ObservableProperty]
    public partial FinalisePhase Phase { get; private set; } = FinalisePhase.None;

    // Position in the delivered audio. One level event arrives per 100 ms of audio
    [ObservableProperty]
    public partial double AudioSeconds { get; private set; }

    /// <summary>The session the engine is recording into, for a resume after a restart.</summary>
    public string? RecordingSessionId { get; private set; }

    /// <summary>Raised when a stop or import seals a session.</summary>
    public event Action<string>? Sealed;

    // With Keep consultations off the engine erases the session once it is left
    internal bool Retain => _preferences.KeepConsultations;

    partial void OnStateChanged(SessionState value)
    {
        _activity.Idle = value == SessionState.Idle;
        if (value == SessionState.Idle)
        {
            _activity.ShowingSample = false;
        }
    }

    /// <summary>Enters review when the note arrives or a stored session opens.</summary>
    public void EnterReview(bool panesOpen = false)
    {
        if (panesOpen)
        {
            Phase = FinalisePhase.Streaming;
        }

        State = SessionState.Review;
    }

    /// <summary>
    /// The engine refused the note. After a stop there is nothing to review, and within a review
    /// a refusal card shows.
    /// </summary>
    public void Refuse() =>
        State = State == SessionState.Finalising ? SessionState.Refused : SessionState.Review;

    /// <summary>Leaves the review or the refusal. The caller clears the panes.</summary>
    public void Idle()
    {
        Phase = FinalisePhase.None;
        State = SessionState.Idle;
    }

    /// <summary>
    /// A finalise stage from the engine. A late stage cannot move the phase backwards.
    /// </summary>
    public void AdvancePhase(FinaliseStage stage)
    {
        if (State != SessionState.Finalising || Phase >= FinalisePhase.Note)
        {
            return;
        }

        Phase = stage switch
        {
            FinaliseStage.Transcript => FinalisePhase.Transcript,
            FinaliseStage.Speakers => FinalisePhase.Speakers,
            FinaliseStage.Turns => FinalisePhase.Turns,
            _ => Phase,
        };
    }

    /// <summary>The first note token opens the panes.</summary>
    public void NoteStreaming() => Phase = FinalisePhase.Streaming;

    /// <summary>The session ended without a stop. The recording is kept.</summary>
    public void Interrupt(string? detail)
    {
        DropSession();
        _panes.Clear();
        _activity.Decoding = false;
        _status.Append(detail is not null
            ? $"Recording interrupted ({detail}) - consultation kept"
            : "Recording interrupted - consultation kept");
    }

    private void DropSession()
    {
        State = SessionState.Idle;
        _activity.StopListening();
    }

    public void OnAudioLevel(AudioLevel level)
    {
        _activity.Level = level.Level;
        if (State != SessionState.Recording)
        {
            return;
        }

        AudioSeconds += 0.1;
    }

    public async Task StartRecordingAsync()
    {
        if (State != SessionState.Idle)
        {
            return;
        }

        // An empty mic id means the default, and a missing device falls back to it with a log line.
        // A refusal gives the reason, such as a model that is not installed
        var started = await EngineCall.TryAsync(_status, "session/start",
            () => _engine.StartSessionAsync(Retain, _preferences.MicId),
            refused: "Recording could not start").ConfigureAwait(true);
        if (started is null)
        {
            return;
        }

        RecordingSessionId = started.Length > 0 ? started : null;
        AudioSeconds = 0;
        Phase = FinalisePhase.None;
        State = SessionState.Recording;
        _activity.Start();
        _activity.Listening = true;
        _status.Append("Recording");
        _metrics.SessionStarted();
    }

    /// <summary>An import begins. The audio is the file's length and nothing is metered
    /// yet.</summary>
    internal void BeginImport(double seconds)
    {
        AudioSeconds = seconds;
        _activity.Start();
    }

    // A restarted engine has lost the live session, but its audio is stored. The engine feeds it
    // into a fresh session ahead of the microphone and recording carries on
    public async Task ResumeAfterRestartAsync()
    {
        var resume = RecordingSessionId;
        if (resume is null)
        {
            return;
        }

        try
        {
            // Raw call so the catch can tell a refusal from a lost engine
            var started = await _engine.ResumeSessionAsync(resume, Retain).ConfigureAwait(true);
            RecordingSessionId = started.Length > 0 ? started : null;
            _status.Append("Recording");
        }
        catch (Exception e) when (e is EngineErrorException or OperationCanceledException)
        {
            // The engine is up and cannot resume, or never answered
            State = SessionState.Idle;
            _activity.StopListening();
            _status.Append("Could not resume - consultation kept");
            _status.Log($"session/start failed: {e.Message}");
        }
        catch (Exception)
        {
            // The engine died again mid-resume. Staying in Recording lets the next reconnect retry
            _status.Append("Recovering", busy: true);
        }
    }

    public async Task StopRecordingAsync()
    {
        if (State != SessionState.Recording)
        {
            return;
        }

        _metrics.StopRequested();
        _activity.StopListening();
        // The recording is safe in the store even when the stop fails
        await FinaliseAsync("session/stop", "Stop failed, consultation kept", _engine.StopSessionAsync)
            .ConfigureAwait(true);
    }

    // The shared tail of stop and import. A failure must not wedge the UI, so it goes back to
    // idle saying why. A cancelled import has nothing to say
    internal async Task FinaliseAsync(string step, string problem, Func<Task<string>> call)
    {
        State = SessionState.Finalising;
        Phase = FinalisePhase.Sealing;
        _activity.Decoding = true;  // the tail decode keeps the RT figure up
        _note.Apply(NotePipelineEvent.NoteWritingStarted);
        _guidance.NoteStarted();
        _status.Append("Finalising", busy: true);
        string sealedId;
        try
        {
            sealedId = await call().ConfigureAwait(true);
        }
        catch (Exception e)
        {
            _status.Log($"{step} failed: {e.Message}");
            State = SessionState.Idle;
            _panes.Clear();
            _activity.Decoding = false;
            _status.Append(e is ImportCancelledException ? "Cancelled" : $"{problem}: {EngineWords.Reason(e)}");
            return;
        }

        // The finalised transcript carries the speaker labels the live feed
        // could not. It replaces the pane once the engine has sealed it
        if (sealedId.Length > 0)
        {
            Sealed?.Invoke(sealedId);
            await LoadFinalTranscriptAsync(sealedId).ConfigureAwait(true);
        }
    }

    public async Task LoadFinalTranscriptAsync(string? id)
    {
        if (string.IsNullOrEmpty(id))
        {
            _activity.Decoding = false;
            Phase = FinalisePhase.Note;  // nothing to fetch, the panes still open
            return;
        }

        try
        {
            var turns = await _store.TranscriptAsync(id).ConfigureAwait(true);
            _transcript.Clear();
            foreach (var turn in turns)
            {
                _transcript.Add(turn.Speaker, turn.FirstFrame, turn.Text);
            }
        }
        catch (Exception)
        {
            _status.Append("Could not load transcript");
        }
        finally
        {
            _activity.Decoding = false;  // sealed, so the tail decode is over
            // A note that began streaming during the fetch keeps its panes
            if (Phase < FinalisePhase.Note)
            {
                Phase = FinalisePhase.Note;
            }
        }
    }

    public async Task CancelRecordingAsync()
    {
        if (State != SessionState.Recording)
        {
            return;
        }

        if (!await EngineCall.TryAsync(_status, "session/cancel", () => _engine.CancelSessionAsync())
            .ConfigureAwait(true))
        {
            return;
        }

        DropSession();
        _status.Append("Cancelled");
    }
}
