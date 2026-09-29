using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Preferences;

namespace ClinicAVT.App.Core.Features.Documents;

public sealed partial class NoteViewModel : ObservableObject
{
    private string _noteSnapshot = "";
    private bool _suppressOptionsChanged;

    public NoteViewModel(AppPreferences preferences)
    {
        // Saved options are applied before any handler subscribes, so restoring them is not a
        // change
        Style = preferences.NoteStyle;
        Detail = preferences.NoteDetail;
    }

    [ObservableProperty]
    public partial NotePipelineState PipelineState { get; private set; } = NotePipelineState.Pending;

    [ObservableProperty]
    public partial string ClinicalNoteText { get; set; } = "";

    /// <summary>The note style as the engine names it, "prose" or "soap".</summary>
    [ObservableProperty]
    public partial string Style { get; set; } = NoteOptions.DefaultStyle.Value;

    /// <summary>"concise" or "detailed".</summary>
    [ObservableProperty]
    public partial string Detail { get; set; } = NoteOptions.DefaultDetail.Value;

    /// <summary>Why the model refused, in its words, empty unless refused.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(NoteRefused))]
    public partial string RefusalReason { get; set; } = "";

    /// <summary>
    /// False when the recording was too short, because insisting would make the model fabricate.
    /// </summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanWriteAnyway))]
    public partial bool WriteAnywayAvailable { get; set; } = true;

    /// <summary>
    /// False when the consultation will not be kept, because a reflection needs its session.
    /// </summary>
    [ObservableProperty]
    public partial bool ReflectAvailable { get; set; } = true;

    /// <summary>True once an appraisal entry exists. The button then reads Open.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ReflectLabel))]
    public partial bool HasReflection { get; set; }

    /// <summary>"Edited 10:31" when a person changed the stored note, empty otherwise.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Edited))]
    public partial string EditedStamp { get; set; } = "";

    /// <summary>Open while Regenerate waits for the clinician to confirm losing an edit.</summary>
    [ObservableProperty]
    public partial bool RegenerateWarningOpen { get; set; }

    // The editing gate. A document changes only between an explicit Edit and its Save.
    // Discard restores the snapshot taken at Edit
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(NoteViewing))]
    [NotifyCanExecuteChangedFor(nameof(EditNoteCommand))]
    public partial bool NoteEditing { get; private set; }

    public IReadOnlyList<NoteOption> StyleOptions { get; } = NoteOptions.Styles;

    public IReadOnlyList<NoteOption> DetailOptions { get; } = NoteOptions.Details;

    /// <summary>Raised when style or detail changes, for persistence.</summary>
    public event Action? OptionsChanged;

    public event Action? Cleared;

    public bool NoteRefused => PipelineState == NotePipelineState.NoteRefused;

    public bool CanWriteAnyway => NoteRefused && WriteAnywayAvailable;

    public string ReflectLabel => HasReflection ? "Open reflection" : "Create reflection";

    public bool Edited => EditedStamp.Length > 0;

    public bool NoteViewing => !NoteEditing;

    /// <summary>True once the document is sealed. Gates save and copy.</summary>
    public bool NoteDocumentReady =>
        PipelineState is NotePipelineState.AllReady or NotePipelineState.PatientFailed;

    public bool NotePreparing =>
        PipelineState == NotePipelineState.NoteWriting && ClinicalNoteText.Length == 0;

    public string NoteStateCaption => PipelineState switch
    {
        NotePipelineState.NoteWriting => "Writing the note",
        NotePipelineState.NoteFailed => "The note could not be written - see the status bar",
        NotePipelineState.NoteRefused => "No note: the recording was too short or did not contain enough clinical information",
        _ => "",
    };

    public bool NoteCaptionVisible => NoteStateCaption.Length > 0;

    [RelayCommand]
    private void KeepEdits() => RegenerateWarningOpen = false;

    [RelayCommand(CanExecute = nameof(CanEditNote))]
    private void EditNote()
    {
        _noteSnapshot = ClinicalNoteText;
        NoteEditing = true;
    }

    private bool CanEditNote() => NoteDocumentReady && !NoteEditing;

    [RelayCommand]
    private void DiscardNote()
    {
        ClinicalNoteText = _noteSnapshot;
        NoteEditing = false;
    }

    public void FinishEditing() => NoteEditing = false;

    public void BeginRegenerate() => ClearDocuments(NotePipelineState.NoteWriting);

    /// <summary>
    /// A stored session's documents, ready for review. The options show the
    /// stored values without becoming the clinician's new defaults.
    /// </summary>
    public void LoadStored(string note, string style, string detail, string editedStamp)
    {
        _suppressOptionsChanged = true;
        try
        {
            if (style.Length > 0)
            {
                Style = style;
            }

            if (detail.Length > 0)
            {
                Detail = detail;
            }
        }
        finally
        {
            _suppressOptionsChanged = false;
        }

        NoteEditing = false;
        ClinicalNoteText = note;
        EditedStamp = editedStamp;
        RegenerateWarningOpen = false;
        PipelineState = NotePipelineState.AllReady;
    }

    /// <summary>Applies an engine-reported event. Out-of-order events are refused.</summary>
    public bool Apply(NotePipelineEvent pipelineEvent)
    {
        var next = (PipelineState, pipelineEvent) switch
        {
            (NotePipelineState.Pending, NotePipelineEvent.NoteWritingStarted)
                => NotePipelineState.NoteWriting,
            (NotePipelineState.NoteWriting, NotePipelineEvent.NoteReady)
                => NotePipelineState.NoteReadyPatientWriting,
            (NotePipelineState.NoteWriting, NotePipelineEvent.NoteFailed)
                => NotePipelineState.NoteFailed,
            (NotePipelineState.NoteWriting, NotePipelineEvent.NoteRefused)
                => NotePipelineState.NoteRefused,
            (NotePipelineState.NoteRefused, NotePipelineEvent.NoteWritingStarted)
                => NotePipelineState.NoteWriting,
            (NotePipelineState.NoteReadyPatientWriting, NotePipelineEvent.PatientInfoReady)
                => NotePipelineState.AllReady,
            (NotePipelineState.NoteReadyPatientWriting, NotePipelineEvent.PatientInfoFailed)
                => NotePipelineState.PatientFailed,
            _ => (NotePipelineState?)null,
        };
        if (next is null)
        {
            return false;
        }

        PipelineState = next.Value;
        return true;
    }

    public void Reset()
    {
        WriteAnywayAvailable = true;
        RegenerateWarningOpen = false;
        ClearDocuments(NotePipelineState.Pending);
    }

    partial void OnPipelineStateChanged(NotePipelineState value)
    {
        EditNoteCommand.NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(NoteDocumentReady));
        OnPropertyChanged(nameof(NotePreparing));
        OnPropertyChanged(nameof(NoteStateCaption));
        OnPropertyChanged(nameof(NoteRefused));
        OnPropertyChanged(nameof(CanWriteAnyway));
        OnPropertyChanged(nameof(NoteCaptionVisible));
    }

    partial void OnClinicalNoteTextChanged(string value) =>
        OnPropertyChanged(nameof(NotePreparing));

    partial void OnStyleChanged(string value) => OptionChanged();

    partial void OnDetailChanged(string value) => OptionChanged();

    // Clears every document and stamp, an open edit included, since the next text replaces them
    private void ClearDocuments(NotePipelineState state)
    {
        NoteEditing = false;
        RefusalReason = "";
        PipelineState = state;
        ClinicalNoteText = "";
        EditedStamp = "";
        Cleared?.Invoke();
    }

    private void OptionChanged()
    {
        if (!_suppressOptionsChanged)
        {
            OptionsChanged?.Invoke();
        }
    }
}
