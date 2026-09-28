using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Features.Demo;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// The consultation as the views see it. One object over the recorder, the review, the
/// engine's readiness and the notification router, forwarding the state and commands of
/// each under their own names.
/// </summary>
public sealed partial class ConsultationViewModel : ObservableObject, ISessionState
{
    private readonly IDialogService _dialogs;

    public ConsultationViewModel(
        IEngineApi engine, IUiDispatcher dispatcher,
        TranscriptViewModel transcript, NoteViewModel note, StatusBarViewModel status,
        IDialogService dialogs, PageViewModel pageView, GuidanceViewModel guidance,
        Metrics.PerformanceCollector? metrics = null, TimeSpan? readinessPollInterval = null,
        AppPreferences? preferences = null, IReadOnlyList<DemoCase>? exampleCases = null)
    {
        _dialogs = dialogs;
        Transcript = transcript;
        Note = note;
        Guidance = guidance;
        PageView = pageView;
        Status = status;
        Recorder = new SessionRecorder(
            engine, status, note, guidance, pageView, transcript, metrics, preferences);
        Review = new SessionReview(
            engine, dispatcher, status, note, guidance, transcript, dialogs, Recorder, preferences);
        Readiness = new ConsultationReadiness(
            engine, status, note, guidance, Recorder, preferences,
            readinessPollInterval ?? TimeSpan.FromSeconds(2));
        var router = new NotificationRouter(Recorder, Review, Readiness, note, guidance, status, metrics);

        // The parts' properties are this object's, under the same names
        Recorder.PropertyChanged += (_, e) => OnPropertyChanged(e.PropertyName);
        Readiness.PropertyChanged += (_, e) => OnPropertyChanged(e.PropertyName);

        EngineReady = engine.Connected;
        Note.TranslateRequested = Review.TranslateAsync;
        Note.RegenerateRequested = Review.RegenerateNoteAsync;
        Note.WriteAnywayRequested = Review.WriteNoteAnywayAsync;
        Note.RegeneratePatientRequested = Review.RegeneratePatientAsync;
        Note.ReflectRequested = Review.ReflectAsync;
        Note.SaveNoteRequested = Review.SaveNoteAsync;
        Note.SavePatientRequested = Review.SavePatientAsync;
        Note.ExampleCases = exampleCases ?? DemoCases.Load();
        Note.ExampleCaseRequested = example => _ = Review.ApplyExampleCaseAsync(example);
        Note.OriginalNoteRequested = () => _ = Review.RestoreOriginalNoteAsync();
        Guidance.SearchNoteRequested = Review.SearchGuidanceAsync;
        Guidance.SearchQueryRequested = Review.SearchGuidanceAsync;
        Guidance.ShowInDocumentRequested = PageView.ShowAsync;
        Guidance.OpenDocumentRequested = PageView.OpenAsync;
        Guidance.CardsShown = () => PageView.KeepOnlyFor(Guidance.Cards);
        // Saved options are applied before the change callback is wired, so restoring them
        // is not itself a change
        if (preferences is not null)
        {
            Note.Style = preferences.NoteStyle;
            Note.Detail = preferences.NoteDetail;
        }

        Note.OptionsChanged = Readiness.NoteOptionsChanged;
        // Off the transport's thread. A handler that throws must not take the others with it
        engine.NotificationReceived += notification => dispatcher.Post(() =>
        {
            try
            {
                router.Route(notification);
            }
            catch (Exception e)
            {
                Status.Log($"{notification.GetType().Name} handler failed: {e.Message}");
            }
        });
        // The status-bar label carries readiness. A lost connection is only logged, because
        // an intentional restart such as the NPU switch must not read as a failure
        engine.ConnectedChanged += connected => dispatcher.Post(() =>
        {
            EngineReady = connected;
            Status.SetEngineReady(connected);
            if (!connected)
            {
                Note.TranslationRunning = false;
                Guidance.ConnectionLost();
                Status.Log("connection lost");
            }
            else
            {
                // Whatever activity the restart interrupted, such as "switching
                // transcription...", is over. Resume overwrites this
                Status.Append("Ready");
                Readiness.Connected();
                if (State == SessionState.Recording)
                {
                    _ = Recorder.ResumeAfterRestartAsync();
                }
            }
        });
        if (EngineReady)
        {
            Readiness.Connected();
        }
    }

    public SessionRecorder Recorder { get; }

    public SessionReview Review { get; }

    public ConsultationReadiness Readiness { get; }

    public TranscriptViewModel Transcript { get; }

    public NoteViewModel Note { get; }

    public GuidanceViewModel Guidance { get; }

    /// <summary>The page of an added document beside the note, when a card asks.</summary>
    public PageViewModel PageView { get; }

    public StatusBarViewModel Status { get; }

    /// <summary>False while the engine is still starting or reconnecting.</summary>
    [ObservableProperty]
    public partial bool EngineReady { get; private set; }

    public SessionState State => Recorder.State;

    public FinalisePhase Phase => Recorder.Phase;

    public bool Paused => Recorder.Paused;

    public double AudioSeconds => Recorder.AudioSeconds;

    public bool Importing => Recorder.Importing;

    public string? ImportLine => Recorder.ImportLine;

    public ReplayRequest? ActiveReplay => Recorder.ActiveReplay;

    public bool ModelsReady => Readiness.ModelsReady;

    public bool ConsultationInProgress => State is SessionState.Recording or SessionState.Finalising;

    public string? ReviewedSessionId =>
        State is SessionState.Review or SessionState.Refused ? Review.FinalisedSessionId : null;

    public Task EndReviewAsync() => CloseReviewAsync();

    /// <summary>
    /// Finalising is the state worth splitting. Its stages differ by an order of magnitude,
    /// so a crash log needs to know which one.
    /// </summary>
    public string SessionPhase =>
        State == SessionState.Finalising ? $"{State}:{Phase}" : State.ToString();

    public Task StartRecordingAsync(ReplayRequest? replay = null) => Recorder.StartRecordingAsync(replay);

    public Task StopRecordingAsync() => Recorder.StopRecordingAsync();

    public Task CancelRecordingAsync() => Recorder.CancelRecordingAsync();

    /// <summary>
    /// The Add consultation recording dialog, on a dropped file when there is one. An open
    /// review ends before the import starts.
    /// </summary>
    public async Task ImportRecordingAsync(string? path = null)
    {
        if (await _dialogs.RunImportAsync(path).ConfigureAwait(true) is not { } import)
        {
            return;
        }

        await CloseReviewAsync().ConfigureAwait(true);
        await Recorder.ImportRecordingAsync(import).ConfigureAwait(true);
    }

    public Task CancelImportAsync() => Recorder.CancelImportAsync();

    public Task SetPausedAsync(bool paused) => Recorder.SetPausedAsync(paused);

    public Task SetMonitorAsync(bool on) => Recorder.SetMonitorAsync(on);

    public Task<bool> OpenStoredSessionAsync(string id, string startedLabel = "",
        string startedAt = "", bool hasReflection = false, bool demo = false) =>
        Review.OpenStoredSessionAsync(id, startedLabel, startedAt, hasReflection, demo);

    public Task CloseReviewAsync() => Review.CloseReviewAsync();

    /// <summary>
    /// True while the review shows a stored consultation. False for the one just recorded.
    /// </summary>
    public bool ReviewingStored => Review.StoredOpen;

    /// <summary>
    /// The id of the consultation just recorded while its review is up. Null once a stored one
    /// replaces it.
    /// </summary>
    public string? LiveReviewId =>
        State == SessionState.Review && !Review.StoredOpen ? Review.FinalisedSessionId : null;

    public Task SaveNoteAsync() => Review.SaveNoteAsync();

    public Task RegenerateNoteAsync() => Review.RegenerateNoteAsync();

    public void FinishConsultation() => _ = CloseReviewAsync();
}
