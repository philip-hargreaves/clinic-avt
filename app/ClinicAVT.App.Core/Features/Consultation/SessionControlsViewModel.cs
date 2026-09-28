using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Shell;

namespace ClinicAVT.App.Core.Features.Consultation;

public sealed partial class SessionControlsViewModel : ObservableObject
{
    private readonly ConsultationViewModel _session;
    private readonly MicViewModel _mic;

    public SessionControlsViewModel(ConsultationViewModel session, MicViewModel mic)
    {
        _session = session;
        _mic = mic;
        _mic.PropertyChanged += (_, _) => OnPropertyChanged(nameof(MicTip));
        _session.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(ConsultationViewModel.State)
                or nameof(ConsultationViewModel.EngineReady)
                or nameof(ConsultationViewModel.Phase)
                or nameof(ConsultationViewModel.ModelsReady)
                or nameof(ConsultationViewModel.Importing)
                or nameof(ConsultationViewModel.ImportLine))
            {
                OnPropertyChanged(nameof(IdleVisible));
                OnPropertyChanged(nameof(RecordingVisible));
                OnPropertyChanged(nameof(ReviewVisible));
                OnPropertyChanged(nameof(MicPickerVisible));
                OnPropertyChanged(nameof(MicPickerEnabled));
                OnPropertyChanged(nameof(MicTip));
                OnPropertyChanged(nameof(CentreStageVisible));
                OnPropertyChanged(nameof(PanesVisible));
                OnPropertyChanged(nameof(FinalisingVisible));
                OnPropertyChanged(nameof(ImportCancelVisible));
                OnPropertyChanged(nameof(RefusedVisible));
                DoneCommand.NotifyCanExecuteChanged();
                OnPropertyChanged(nameof(FinalisingLabel));
                OnPropertyChanged(nameof(StartLabel));
                StartRecordingCommand.NotifyCanExecuteChanged();
                ImportRecordingCommand.NotifyCanExecuteChanged();
                StopRecordingCommand.NotifyCanExecuteChanged();
                CancelRecordingCommand.NotifyCanExecuteChanged();
                CancelImportCommand.NotifyCanExecuteChanged();
                FinishConsultationCommand.NotifyCanExecuteChanged();
            }
            else if (e.PropertyName is nameof(ConsultationViewModel.AudioSeconds))
            {
                OnPropertyChanged(nameof(ElapsedLabel));
            }
        };
        _session.Status.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(StatusBarViewModel.MicLevel))
            {
                OnPropertyChanged(nameof(Level));
            }
            else if (e.PropertyName is nameof(StatusBarViewModel.ModelLoadLine))
            {
                OnPropertyChanged(nameof(FinalisingLabel));
            }
        };
    }

    /// <summary>Microphone level, 0 to 1, for the ring around the disc.</summary>
    public double Level => _session.Status.MicLevel;

    /// <summary>The centre-stage caption for the current finalise phase. An import shows its own
    /// stage and percentage until it is sealed. A note that waits on the note model's load says
    /// so, with the time.</summary>
    public string FinalisingLabel => _session.Phase switch
    {
        < FinalisePhase.Note when _session.Importing => _session.ImportLine ?? "Preparing",
        FinalisePhase.Transcript => "Writing transcript",
        FinalisePhase.Speakers => "Labelling speakers",
        FinalisePhase.Turns => "Writing transcript",
        FinalisePhase.Note when _session.Status.ModelLoading =>
            $"Waiting for the note model · {_session.Status.ModelLoadElapsed}",
        FinalisePhase.Note => "Preparing note",
        _ => "Finalising",
    };

    // The view swaps by state. Computed here so it is testable
    public bool IdleVisible => _session.State == SessionState.Idle;

    public bool RecordingVisible => _session.State == SessionState.Recording;

    public bool ReviewVisible => _session.State == SessionState.Review;

    public bool RefusedVisible => _session.State == SessionState.Refused;

    /// <summary>The refusal card reads its reason and override from here.</summary>
    public NoteViewModel Note => _session.Note;

    // Derived from ReviewVisible, so the picker and New consultation never show together
    public bool MicPickerVisible => !ReviewVisible;

    /// <summary>
    /// The device is pinned once recording starts. A change applies to the next consultation.
    /// </summary>
    public bool MicPickerEnabled => _session.State == SessionState.Idle;

    public string MicTip => MicPickerEnabled
        ? _mic.FullName
        : "In use - changes apply to the next consultation";

    // The centre holds until the note streams. Panes and centre never show together
    public bool CentreStageVisible =>
        _session.State is SessionState.Idle or SessionState.Recording or SessionState.Refused
        || _session.State == SessionState.Finalising && _session.Phase != FinalisePhase.Streaming;

    public bool PanesVisible => !CentreStageVisible;

    public bool FinalisingVisible =>
        _session.State == SessionState.Finalising && _session.Phase != FinalisePhase.Streaming;

    /// <summary>An import can be stopped until the engine has sealed it.</summary>
    public bool ImportCancelVisible => FinalisingVisible && _session.Importing;

    public string ElapsedLabel => Words.Position(_session.AudioSeconds);

    public string StartLabel =>
        _session.State == SessionState.Idle && !(_session.EngineReady && _session.ModelsReady)
            ? "Getting ready"
            : "Ready to start";

    [RelayCommand(CanExecute = nameof(CanStartRecording))]
    private Task StartRecording() => _session.StartRecordingAsync();

    private bool CanStartRecording() =>
        _session.State == SessionState.Idle && _session.EngineReady && _session.ModelsReady;

    /// <summary>Opens the import dialog, on the dropped file when given one.</summary>
    [RelayCommand(CanExecute = nameof(CanImportRecording))]
    private Task ImportRecording(string? path) => _session.ImportRecordingAsync(path);

    // From idle, or from a review the import then ends
    private bool CanImportRecording() =>
        _session.State is SessionState.Idle or SessionState.Review or SessionState.Refused
        && _session.EngineReady && _session.ModelsReady;

    [RelayCommand(CanExecute = nameof(CanStopRecording))]
    private Task StopRecording() => _session.StopRecordingAsync();

    private bool CanStopRecording() => _session.State == SessionState.Recording;

    [RelayCommand(CanExecute = nameof(CanCancelRecording))]
    private Task CancelRecording() => _session.CancelRecordingAsync();

    private bool CanCancelRecording() => _session.State == SessionState.Recording;

    [RelayCommand(CanExecute = nameof(ImportCancelVisible))]
    private Task CancelImport() => _session.CancelImportAsync();

    [RelayCommand(CanExecute = nameof(CanFinishConsultation))]
    private void FinishConsultation() => _session.FinishConsultation();

    private bool CanFinishConsultation() => _session.State == SessionState.Review;

    [RelayCommand(CanExecute = nameof(CanDone))]
    private Task Done() => _session.CloseReviewAsync();

    private bool CanDone() => _session.State == SessionState.Refused;
}
