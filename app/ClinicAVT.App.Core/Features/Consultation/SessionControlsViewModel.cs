using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Features.Consultation;

public sealed partial class SessionControlsViewModel : ObservableObject
{
    private readonly IConsultation _session;
    private readonly MicViewModel _mic;
    private readonly ConsultationActivity _activity;
    private readonly INoteModelLoad _load;

    public SessionControlsViewModel(
        IConsultation session, MicViewModel mic, ConsultationActivity activity, INoteModelLoad load,
        NoteViewModel note, ReviewCommandsViewModel commands)
    {
        _session = session;
        _mic = mic;
        _activity = activity;
        _load = load;
        Note = note;
        Commands = commands;
        _mic.PropertyChanged += (_, _) => OnPropertyChanged(nameof(MicTip));
        _session.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(IConsultation.State)
                or nameof(IConsultation.EngineReady)
                or nameof(IConsultation.Phase)
                or nameof(IConsultation.ModelsReady)
                or nameof(IConsultation.Importing)
                or nameof(IConsultation.ImportLine))
            {
                OnPropertyChanged(nameof(IdleVisible));
                OnPropertyChanged(nameof(RecordingVisible));
                OnPropertyChanged(nameof(ReviewVisible));
                OnPropertyChanged(nameof(MicPickerVisible));
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
            else if (e.PropertyName is nameof(IConsultation.AudioSeconds))
            {
                OnPropertyChanged(nameof(ElapsedLabel));
            }
        };
        _activity.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(ConsultationActivity.Level))
            {
                OnPropertyChanged(nameof(Level));
            }
        };
        _load.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(INoteModelLoad.ModelLoadLine))
            {
                OnPropertyChanged(nameof(FinalisingLabel));
            }
        };
    }

    /// <summary>Microphone level, 0 to 1, for the ring around the disc.</summary>
    public double Level => _activity.Level;

    /// <summary>The centre-stage caption for the finalise phase. An import shows its own progress
    /// until it is sealed.</summary>
    public string FinalisingLabel => _session.Phase switch
    {
        < FinalisePhase.Note when _session.Importing => _session.ImportLine ?? "Preparing",
        FinalisePhase.Transcript => "Writing transcript",
        FinalisePhase.Speakers => "Labelling speakers",
        FinalisePhase.Turns => "Writing transcript",
        FinalisePhase.Note when _load.ModelLoading =>
            $"Waiting for the note model · {_load.ModelLoadElapsed}",
        FinalisePhase.Note => "Preparing note",
        _ => "Finalising",
    };

    // The view swaps by state. Computed here so it is testable
    public bool IdleVisible => _session.State == SessionState.Idle;

    public bool RecordingVisible => _session.State == SessionState.Recording;

    public bool ReviewVisible => _session.State == SessionState.Review;

    public bool RefusedVisible => _session.State == SessionState.Refused;

    /// <summary>The refusal card reads its reason and override from here.</summary>
    public NoteViewModel Note { get; }

    public ReviewCommandsViewModel Commands { get; }

    // Shown only when idle. The mic is fixed from Record, and Finish consultation takes this slot
    // in review
    public bool MicPickerVisible => _session.State == SessionState.Idle;

    public string MicTip => _mic.FullName;

    // Shown until the note starts streaming, never together with the panes
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

    [RelayCommand(CanExecute = nameof(CanImportRecording))]
    private Task ImportRecording(string? path) => _session.ImportRecordingAsync(path);

    // An import can also start from review, which it ends
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
