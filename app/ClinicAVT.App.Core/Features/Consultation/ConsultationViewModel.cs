using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// Facade for the views over the recorder, review, readiness and notification router. Forwards
/// their state and commands under the same names.
/// </summary>
public sealed partial class ConsultationViewModel : ObservableObject, IConsultation
{
    private readonly IStatusLine _status;
    private readonly SessionRecorder _recorder;
    private readonly SessionImport _import;
    private readonly SessionReview _review;
    private readonly ConsultationReadiness _readiness;
    private readonly IDialogService _dialogs;
    private readonly Func<ImportRecordingViewModel> _imports;

    public ConsultationViewModel(
        IEngineEvents events, IStatusLine status, SessionRecorder recorder, SessionImport import,
        SessionReview review, ConsultationReadiness readiness, NotificationRouter router,
        IDialogService dialogs, Func<ImportRecordingViewModel> imports)
    {
        _status = status;
        _recorder = recorder;
        _import = import;
        _review = review;
        _readiness = readiness;
        _dialogs = dialogs;
        _imports = imports;

        recorder.PropertyChanged += (_, e) => OnPropertyChanged(e.PropertyName);
        import.PropertyChanged += (_, e) => OnPropertyChanged(e.PropertyName);
        readiness.PropertyChanged += (_, e) => OnPropertyChanged(e.PropertyName);

        EngineReady = events.Connected;
        events.Subscribe<EngineNotification>(router.Route);
        // The status-bar label carries readiness. A lost connection is only logged, because
        // an intentional restart such as the NPU switch must not read as a failure
        events.SubscribeConnection(connected =>
        {
            EngineReady = connected;
            if (!connected)
            {
                _status.Log("connection lost");
            }
            else
            {
                // Readiness says "Ready" once the engine confirms the models are prepared
                _readiness.Connected();
                if (State == SessionState.Recording)
                {
                    _ = _recorder.ResumeAfterRestartAsync();
                }
            }
        });
        if (EngineReady)
        {
            _readiness.Connected();
        }
    }

    public event Action<string>? Sealed
    {
        add => _recorder.Sealed += value;
        remove => _recorder.Sealed -= value;
    }

    public event Action<RecordingImport>? ImportStarted
    {
        add => _import.ImportStarted += value;
        remove => _import.ImportStarted -= value;
    }

    /// <summary>False while the engine is still starting or reconnecting.</summary>
    [ObservableProperty]
    public partial bool EngineReady { get; private set; }

    public SessionState State => _recorder.State;

    public FinalisePhase Phase => _recorder.Phase;

    public double AudioSeconds => _recorder.AudioSeconds;

    public string? RecordingSessionId => _recorder.RecordingSessionId;

    public bool Importing => _import.Importing;

    public string? ImportLine => _import.ImportLine;

    public bool ModelsReady => _readiness.ModelsReady;

    public bool ConsultationInProgress => State is SessionState.Recording or SessionState.Finalising;

    public string? ReviewedSessionId =>
        State is SessionState.Review or SessionState.Refused ? _review.FinalisedSessionId : null;

    public Task EndReviewAsync() => CloseReviewAsync();

    /// <summary>
    /// State for the crash log. Finalising includes its phase, as its stages differ in length by an
    /// order of magnitude.
    /// </summary>
    public string SessionPhase =>
        State == SessionState.Finalising ? $"{State}:{Phase}" : State.ToString();

    /// <summary>
    /// True while the review shows a stored consultation. False for the one just recorded.
    /// </summary>
    public bool ReviewingStored => _review.StoredOpen;

    /// <summary>
    /// The id of the consultation just recorded while its review is up. Null once a stored one
    /// replaces it.
    /// </summary>
    public string? LiveReviewId =>
        State == SessionState.Review && !_review.StoredOpen ? _review.FinalisedSessionId : null;

    public Task StartRecordingAsync() => _recorder.StartRecordingAsync();

    public Task StopRecordingAsync() => _recorder.StopRecordingAsync();

    public Task CancelRecordingAsync() => _recorder.CancelRecordingAsync();

    /// <summary>
    /// The Add consultation recording dialog, on a dropped file when there is one. An open
    /// review ends before the import starts.
    /// </summary>
    public async Task ImportRecordingAsync(string? path = null)
    {
        var dialog = _imports();
        if (path is not null)
        {
            _ = dialog.UseFileAsync(path);
        }

        // Add closes the dialog and the consultation page shows the finalise
        if (!await _dialogs.ShowAsync(dialog).ConfigureAwait(true) || dialog.Result is not { } import)
        {
            return;
        }

        await CloseReviewAsync().ConfigureAwait(true);
        await _import.ImportRecordingAsync(import).ConfigureAwait(true);
    }

    public Task CancelImportAsync() => _import.CancelImportAsync();

    public Task<bool> OpenStoredSessionAsync(string id, string startedLabel = "",
        string startedAt = "", bool hasReflection = false, bool sample = false) =>
        _review.OpenStoredSessionAsync(id, startedLabel, startedAt, hasReflection, sample);

    public Task CloseReviewAsync() => _review.CloseReviewAsync();

    public void FinishConsultation() => _ = CloseReviewAsync();
}
