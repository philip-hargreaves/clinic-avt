using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Demo;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// The live session. It holds the state machine from Idle through Recording and Finalising,
/// the starts and stops, and the resume after an engine restart. Review and the notification
/// router move the state through the transitions declared here.
/// </summary>
public sealed partial class SessionRecorder : ObservableObject
{
    private readonly IEngineApi _engine;
    private readonly StatusBarViewModel _status;
    private readonly NoteViewModel _note;
    private readonly GuidanceViewModel _guidance;
    private readonly PageViewModel _pageView;
    private readonly TranscriptViewModel _transcript;
    private readonly Metrics.PerformanceCollector? _metrics;
    private readonly AppPreferences? _preferences;
    private readonly DemoMode? _demo;

    // A replay stops itself at the end of its file. Zero when the length is unknown
    private double _replayEndSeconds;

    public SessionRecorder(
        IEngineApi engine, StatusBarViewModel status, NoteViewModel note, GuidanceViewModel guidance,
        PageViewModel pageView, TranscriptViewModel transcript,
        Metrics.PerformanceCollector? metrics, AppPreferences? preferences, DemoMode? demo)
    {
        _engine = engine;
        _status = status;
        _note = note;
        _guidance = guidance;
        _pageView = pageView;
        _transcript = transcript;
        _metrics = metrics;
        _preferences = preferences;
        _demo = demo;
    }

    [ObservableProperty]
    public partial SessionState State { get; private set; } = SessionState.Idle;

    /// <summary>
    /// Where a stop has got to. The centre stage holds until the note streams,
    /// so the note prefill pause shows a spinner that says so.
    /// Advances only forwards within one stop.
    /// </summary>
    [ObservableProperty]
    public partial FinalisePhase Phase { get; private set; } = FinalisePhase.None;

    [ObservableProperty]
    public partial bool Paused { get; private set; }

    // Position in the delivered audio. One level event arrives per 100 ms of audio at any
    // replay speed
    [ObservableProperty]
    public partial double AudioSeconds { get; private set; }

    /// <summary>The active session's replay request, null for a microphone.</summary>
    [ObservableProperty]
    public partial ReplayRequest? ActiveReplay { get; private set; }

    /// <summary>The saved run being played back, null unless a demo plays.</summary>
    [ObservableProperty]
    public partial DemoMaster? ActivePlayback { get; private set; }

    /// <summary>
    /// True for a demo record until the next idle. The badge shows for it and for demo mode.
    /// </summary>
    public bool DemoRecord { get; private set; }

    /// <summary>The session the engine is recording into, for a resume after a restart.</summary>
    public string? RecordingSessionId { get; private set; }

    /// <summary>A stop sealed this session. The review takes it over.</summary>
    public event Action<string>? Sealed;

    public void ShowDemo(bool record)
    {
        DemoRecord = record;
        _status.Demo = record || _demo is { Enabled: true };
        _note.ExampleCasesVisible = record && _note.ExampleCases.Count > 0;
        if (!record)
        {
            _note.ExampleCaseIndex = -1;
        }
    }

    partial void OnStateChanged(SessionState value)
    {
        if (value == SessionState.Idle)
        {
            ShowDemo(false);
        }
    }

    /// <summary>
    /// The note arrived or a stored session opened, so the panes show and review begins.
    /// </summary>
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
    public void AdvancePhase(string stage)
    {
        if (State != SessionState.Finalising || Phase >= FinalisePhase.Note)
        {
            return;
        }

        Phase = stage switch
        {
            "transcript" => FinalisePhase.Transcript,
            "speakers" => FinalisePhase.Speakers,
            "turns" => FinalisePhase.Turns,
            _ => Phase,
        };
    }

    /// <summary>The first note token opens the panes.</summary>
    public void NoteStreaming() => Phase = FinalisePhase.Streaming;

    /// <summary>The session ended without a stop. The recording is kept.</summary>
    public void Interrupt(string? detail)
    {
        DropSession();
        ClearPanes();
        _status.SetDecodeActive(false);
        _status.Append(detail is not null
            ? $"Recording interrupted ({detail}) - consultation kept"
            : "Recording interrupted - consultation kept");
    }

    /// <summary>Empties the note, the guidance and the page view.</summary>
    public void ClearPanes()
    {
        _note.Reset();
        _guidance.Reset();
        _pageView.Hide();
    }

    // Back to idle with nothing of the live session left
    private void DropSession()
    {
        State = SessionState.Idle;
        Paused = false;
        ActiveReplay = null;
        ActivePlayback = null;
        _status.SetMicVisible(false);
    }

    /// <summary>
    /// A level reading. A playback's reading carries the position its clock has reached.
    /// </summary>
    public void OnAudioLevel(AudioLevel level)
    {
        _status.SetMicLevel(level.Level);
        if (State != SessionState.Recording)
        {
            return;
        }

        AudioSeconds = level.Seconds ?? AudioSeconds + 0.1;
        // A playback or a replay stops itself at the end of its audio
        var end = ActivePlayback?.AudioSeconds ?? _replayEndSeconds;
        if (end > 0 && AudioSeconds >= end - 0.05)
        {
            _ = StopRecordingAsync();
        }
    }

    public async Task StartRecordingAsync(ReplayRequest? replay = null)
    {
        if (State != SessionState.Idle)
        {
            return;
        }

        // In demo mode the record button plays the chosen saved run back
        if (replay is null && _demo is { Enabled: true } && _demo.Master is { } master)
        {
            await StartPlaybackAsync(master).ConfigureAwait(true);
            return;
        }

        // With Keep consultations off the engine erases the session once it is left. An empty
        // mic id means the default, and a missing device falls back to it with a log line
        var retain = _preferences?.KeepConsultations ?? true;
        var start = replay is null
            ? () => _engine.StartSessionAsync(retain, _preferences?.MicId ?? "")
            : (Func<Task<string>>)(() => _engine.StartReplayAsync(retain, replay));
        if (!await BeginAsync(start, replay, null).ConfigureAwait(true))
        {
            return;
        }

        _status.Append(replay is null ? "Recording" : "Replaying");
        _metrics?.SessionStarted(
            replay is null ? "mic" : "replay", replay?.Speed ?? 0,
            replay is null ? null : Path.GetFileNameWithoutExtension(replay.Path));
    }

    /// <summary>
    /// Plays a stored consultation back as a demo. It passes through the same states, sped up,
    /// and generates nothing. Performance measurement ignores it.
    /// </summary>
    public async Task StartPlaybackAsync(DemoMaster playback)
    {
        if (State != SessionState.Idle)
        {
            return;
        }

        var start = () => _engine.StartPlaybackAsync(playback.SessionId);
        if (!await BeginAsync(start, null, playback).ConfigureAwait(true))
        {
            return;
        }

        ShowDemo(true);
        _status.Append("Recording");
    }

    private async Task<bool> BeginAsync(
        Func<Task<string>> start, ReplayRequest? replay, DemoMaster? playback)
    {
        var started = await EngineCall.TryAsync(_status, "session/start", start).ConfigureAwait(true);
        if (started is null)
        {
            return false;
        }

        RecordingSessionId = started.Length > 0 ? started : null;
        Paused = false;
        AudioSeconds = 0;
        Phase = FinalisePhase.None;
        ActiveReplay = replay;
        _replayEndSeconds = replay is null ? 0 : DemoTracks.DurationSeconds(replay.Path);
        ActivePlayback = playback;
        State = SessionState.Recording;
        _status.ClearStorageFault();
        _status.ResetThroughput();
        _status.SetMicVisible(true);
        return true;
    }

    // A restarted engine has lost the live session, but its audio is stored. Resume replays it
    // into a fresh session and recording carries on
    public async Task ResumeAfterRestartAsync()
    {
        var resume = RecordingSessionId;
        if (resume is null)
        {
            return;
        }

        // A demo has no audio to resume from
        if (ActivePlayback is not null)
        {
            State = SessionState.Idle;
            ActivePlayback = null;
            _status.SetMicVisible(false);
            _status.Append("Playback interrupted");
            return;
        }

        try
        {
            var retain = _preferences?.KeepConsultations ?? true;
            // The raw call tells a lost engine from one that answered
            var started = await _engine.ResumeSessionAsync(resume, retain, ActiveReplay)
                .ConfigureAwait(true);
            RecordingSessionId = started.Length > 0 ? started : null;
            _status.Append("Recording");
        }
        catch (Exception e) when (e is EngineErrorException or OperationCanceledException)
        {
            // The engine is up and cannot resume, or never answered
            State = SessionState.Idle;
            _status.SetMicVisible(false);
            _status.Append("Could not resume - consultation kept");
            _status.Log($"session/start failed: {e.Message}");
        }
        catch (Exception)
        {
            // The engine died again mid-resume. Staying in Recording lets the next reconnect retry
            _status.Append("Recovering", busy: true);
        }
    }

    public async Task SetPausedAsync(bool paused)
    {
        if (State != SessionState.Recording || paused == Paused)
        {
            return;
        }

        if (await EngineCall.TryAsync(_status, "session/pause", () => _engine.PauseSessionAsync(paused))
            .ConfigureAwait(true))
        {
            Paused = paused;
        }
    }

    public async Task SetMonitorAsync(bool on)
    {
        if (State == SessionState.Recording)
        {
            await EngineCall.TryAsync(_status, "session/monitor", () => _engine.MonitorSessionAsync(on))
                .ConfigureAwait(true);
        }
    }

    public async Task StopRecordingAsync()
    {
        if (State != SessionState.Recording)
        {
            return;
        }

        State = SessionState.Finalising;
        Phase = FinalisePhase.Sealing;
        Paused = false;
        ActiveReplay = null;
        // A playback's timings are staged, so the collector never sees them
        if (ActivePlayback is null)
        {
            _metrics?.StopRequested();
        }

        ActivePlayback = null;
        _status.SetMicVisible(false);
        _status.SetDecodeActive(true);  // the tail decode keeps the RT figure up
        _note.Apply(NotePipelineEvent.NoteWritingStarted);
        _guidance.NoteStarted();
        _status.Append("Finalising", busy: true);
        var stopped = await EngineCall.TryAsync(_status, "session/stop", () => _engine.StopSessionAsync())
            .ConfigureAwait(true);
        if (stopped is null)
        {
            // A failed stop must not wedge the UI. The recording is safe in
            // the store either way
            State = SessionState.Idle;
            ClearPanes();
            _status.SetDecodeActive(false);
            _status.Append("Stop failed - consultation kept");
            return;
        }

        // The finalised transcript carries the speaker labels the live feed
        // could not. It replaces the pane once the engine has sealed it
        if (stopped.Length > 0)
        {
            Sealed?.Invoke(stopped);
            await LoadFinalTranscriptAsync(stopped).ConfigureAwait(true);
        }
    }

    public async Task LoadFinalTranscriptAsync(string? id)
    {
        if (string.IsNullOrEmpty(id))
        {
            _status.SetDecodeActive(false);
            Phase = FinalisePhase.Note;  // nothing to fetch, the panes still open
            return;
        }

        try
        {
            var turns = await _engine.TranscriptAsync(id).ConfigureAwait(true);
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
            _status.SetDecodeActive(false);  // sealed, so the tail decode is over
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
