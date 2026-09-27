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
                or nameof(ConsultationViewModel.ModelsReady))
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
                OnPropertyChanged(nameof(RefusedVisible));
                DoneCommand.NotifyCanExecuteChanged();
                OnPropertyChanged(nameof(FinalisingLabel));
                OnPropertyChanged(nameof(StartLabel));
                StartRecordingCommand.NotifyCanExecuteChanged();
                StopRecordingCommand.NotifyCanExecuteChanged();
                CancelRecordingCommand.NotifyCanExecuteChanged();
                NewConsultationCommand.NotifyCanExecuteChanged();
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

    /// <summary>The centre-stage caption for the current finalise phase. A note that waits on
    /// the note model's load says so, with the time.</summary>
    public string FinalisingLabel => _session.Phase switch
    {
        FinalisePhase.Transcript => "Writing transcript",
        FinalisePhase.Speakers => "Labelling speakers",
        FinalisePhase.Turns => "Writing transcript",
        FinalisePhase.Note when _session.Status.ModelLoading =>
            $"Waiting for the note model to load · {_session.Status.ModelLoadElapsed}",
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

    public string ElapsedLabel => Words.Position(_session.AudioSeconds);

    public string StartLabel =>
        _session.State == SessionState.Idle && !(_session.EngineReady && _session.ModelsReady)
            ? "Getting ready"
            : "Ready to start recording";

    [RelayCommand(CanExecute = nameof(CanStartRecording))]
    private Task StartRecording() => _session.StartRecordingAsync();

    private bool CanStartRecording() =>
        _session.State == SessionState.Idle && _session.EngineReady && _session.ModelsReady;

    [RelayCommand(CanExecute = nameof(CanStopRecording))]
    private Task StopRecording() => _session.StopRecordingAsync();

    private bool CanStopRecording() => _session.State == SessionState.Recording;

    [RelayCommand(CanExecute = nameof(CanCancelRecording))]
    private Task CancelRecording() => _session.CancelRecordingAsync();

    private bool CanCancelRecording() => _session.State == SessionState.Recording;

    [RelayCommand(CanExecute = nameof(CanNewConsultation))]
    private void NewConsultation() => _session.StartNewConsultation();

    private bool CanNewConsultation() => _session.State == SessionState.Review;

    [RelayCommand(CanExecute = nameof(CanDone))]
    private Task Done() => _session.CloseReviewAsync();

    private bool CanDone() => _session.State == SessionState.Refused;
}
