using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Features.Demo;
using ClinicAVT.App.Core.Preferences;

namespace ClinicAVT.App.Core.Features.Documents;

public sealed partial class NoteViewModel : ObservableObject
{
    public const string OriginalNoteTitle = "Original note";

    private string _noteSnapshot = "";
    private string _patientSnapshot = "";
    private bool _suppressOptionsChanged;

    [ObservableProperty]
    public partial NotePipelineState PipelineState { get; private set; } = NotePipelineState.Pending;

    [ObservableProperty]
    public partial string ClinicalNoteText { get; set; } = "";

    [ObservableProperty]
    public partial string PatientInfoText { get; set; } = "";

    [ObservableProperty]
    public partial string TranslationText { get; set; } = "";

    /// <summary>The translation's language, such as "Polish". It heads the output box.</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(TranslateAgainCommand))]
    [NotifyPropertyChangedFor(nameof(TranslationRightToLeft))]
    public partial string TranslationLanguage { get; set; } = "";

    // By the names the translation model lists
    private static readonly HashSet<string> RightToLeftLanguages = ["Arabic", "Farsi", "Kurdish (Sorani)", "Urdu"];

    /// <summary>The translation reads right to left, so its box flows that way.</summary>
    public bool TranslationRightToLeft => RightToLeftLanguages.Contains(TranslationLanguage);

    [ObservableProperty]
    public partial bool ExampleCasesVisible { get; set; }

    /// <summary>
    /// The picker's selection, where -1 shows its placeholder. Choosing applies the case.
    /// </summary>
    [ObservableProperty]
    public partial int ExampleCaseIndex { get; set; } = -1;

    /// <summary>The note style as the engine names it, "prose" or "soap".</summary>
    [ObservableProperty]
    public partial string Style { get; set; } = NoteOptions.DefaultStyle.Value;

    /// <summary>"concise" or "detailed".</summary>
    [ObservableProperty]
    public partial string Detail { get; set; } = NoteOptions.DefaultDetail.Value;

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(TranslateCommand))]
    public partial string? SelectedLanguage { get; set; }

    /// <summary>Why the model refused, in its words, empty unless refused.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(NoteRefused))]
    public partial string RefusalReason { get; set; } = "";

    /// <summary>
    /// False when the recording was too short, because insisting would make the model fabricate.
    /// </summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanWriteAnyway))]
    [NotifyCanExecuteChangedFor(nameof(WriteAnywayCommand))]
    public partial bool WriteAnywayAvailable { get; set; } = true;

    /// <summary>
    /// False when the consultation will not be kept, because a reflection needs its session.
    /// </summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(ReflectCommand))]
    public partial bool ReflectAvailable { get; set; } = true;

    /// <summary>True once an appraisal entry exists. The button then reads Open.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ReflectLabel))]
    public partial bool HasReflection { get; set; }

    /// <summary>True from the request until translate/ready or translate/failed.</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(TranslateCommand))]
    [NotifyCanExecuteChangedFor(nameof(TranslateAgainCommand))]
    public partial bool TranslationRunning { get; set; }

    /// <summary>"Edited 10:31" when a person changed the stored note, empty otherwise.</summary>
    [ObservableProperty]
    public partial string EditedStamp { get; set; } = "";

    /// <summary>
    /// The note has been edited since the sheet was written from it, so the sheet may not say
    /// what the note says. Cleared when the sheet is rewritten.
    /// </summary>
    [ObservableProperty]
    public partial bool PatientStale { get; set; }

    /// <summary>The sheet changed after it was translated. Cleared by a new translation.</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(TranslateAgainCommand))]
    public partial bool TranslationStale { get; set; }

    /// <summary>Open while Regenerate waits for the clinician to confirm losing an edit.</summary>
    [ObservableProperty]
    public partial bool RegenerateWarningOpen { get; set; }

    // The editing gate. A document changes only between an explicit Edit and its Save.
    // Discard restores the snapshot taken at Edit
    [ObservableProperty]
    public partial bool NoteEditing { get; private set; }

    [ObservableProperty]
    public partial bool PatientEditing { get; private set; }

    public string TranslationCaption =>
        TranslationLanguage.Length > 0 ? $"{TranslationLanguage} translation" : "Translation";

    /// <summary>Example cases, offered on a demo record in place of the note.</summary>
    public IReadOnlyList<DemoCase> ExampleCases { get; set; } = [];

    /// <summary>The picker's entries, the stored note first and then the cases.</summary>
    public IReadOnlyList<string> ExampleCaseTitles =>
        [OriginalNoteTitle, .. ExampleCases.Select(c => c.Title)];

    /// <summary>Set by the consultation view model, which owns the engine.</summary>
    public Action<DemoCase>? ExampleCaseRequested { get; set; }

    public Action? OriginalNoteRequested { get; set; }

    public IReadOnlyList<NoteOption> StyleOptions { get; } = NoteOptions.Styles;

    public IReadOnlyList<NoteOption> DetailOptions { get; } = NoteOptions.Details;

    public ObservableCollection<string> Languages { get; } = [];

    /// <summary>Set by the consultation view model, which owns the engine.</summary>
    public Func<string, Task>? TranslateRequested { get; set; }

    public Func<Task>? RegenerateRequested { get; set; }

    /// <summary>The clinician overrides a refusal. The session view model wires it.</summary>
    public Func<Task>? WriteAnywayRequested { get; set; }

    public Func<Task>? RegeneratePatientRequested { get; set; }

    /// <summary>Opens the appraisal reflection for the consultation on screen.</summary>
    public Func<Task>? ReflectRequested { get; set; }

    public Func<Task>? SaveNoteRequested { get; set; }

    public Func<Task>? SavePatientRequested { get; set; }

    /// <summary>Raised when style or detail changes, for persistence.</summary>
    public Action? OptionsChanged { get; set; }

    public bool NoteRefused => PipelineState == NotePipelineState.NoteRefused;

    public bool CanWriteAnyway => NoteRefused && WriteAnywayAvailable;

    public string ReflectLabel => HasReflection ? "Open reflection" : "Create reflection";

    public bool Edited => EditedStamp.Length > 0;

    public bool NoteViewing => !NoteEditing;

    public bool PatientViewing => !PatientEditing;

    public bool AnyEditing => NoteEditing || PatientEditing;

    /// <summary>True once the document is sealed. Gates save and copy.</summary>
    public bool NoteDocumentReady =>
        PipelineState is NotePipelineState.AllReady or NotePipelineState.PatientFailed;

    public bool PatientDocumentReady => PipelineState == NotePipelineState.AllReady;

    // The panes show a quiet affordance while a document is being prepared
    // and nothing has streamed yet. Computed here so it is testable
    public bool NotePreparing =>
        PipelineState == NotePipelineState.NoteWriting && ClinicalNoteText.Length == 0;

    public bool PatientPreparing =>
        PipelineState is NotePipelineState.NoteWriting or NotePipelineState.NoteReadyPatientWriting
        && PatientInfoText.Length == 0;

    public string NoteStateCaption => PipelineState switch
    {
        NotePipelineState.NoteWriting => "Writing the note",
        NotePipelineState.NoteFailed => "The note could not be written - see the status bar",
        NotePipelineState.NoteRefused => "No note: the recording was too short or did not contain enough clinical information",
        _ => "",
    };

    public string PatientStateCaption => PipelineState switch
    {
        NotePipelineState.NoteWriting or NotePipelineState.NoteReadyPatientWriting =>
            "Patient information follows the note",
        NotePipelineState.PatientFailed =>
            "Patient information could not be written - see the status bar",
        _ => "",
    };

    public bool NoteCaptionVisible => NoteStateCaption.Length > 0;

    public bool PatientCaptionVisible => PatientStateCaption.Length > 0;

    // The output box shows only while translating or holding a result
    public bool TranslationVisible => TranslationRunning || TranslationText.Length > 0;

    /// <summary>Copy and Export take the sheet, with its translation once there is one.</summary>
    public string PatientCopyTip =>
        TranslationText.Length > 0 ? "Copies the patient information and its translation" : "Copies the patient information";

    public string PatientExportTip =>
        TranslationText.Length > 0 ? "Saves the patient information and its translation as a text file" : "Saves the patient information as a text file";

    [RelayCommand(CanExecute = nameof(CanWriteAnyway))]
    private Task WriteAnyway() => WriteAnywayRequested!();

    [RelayCommand(CanExecute = nameof(CanReflect))]
    private Task Reflect() => ReflectRequested!();

    private bool CanReflect() => ReflectRequested is not null && ReflectAvailable && NoteDocumentReady;

    /// <summary>
    /// The staleness hint's action, which rewrites the sheet from the edited note.
    /// </summary>
    [RelayCommand(CanExecute = nameof(CanRegeneratePatient))]
    private Task RegeneratePatient() => RegeneratePatientRequested!();

    private bool CanRegeneratePatient() =>
        RegeneratePatientRequested is not null && PatientStale && !AnyEditing;

    [RelayCommand]
    private async Task ConfirmRegenerate()
    {
        RegenerateWarningOpen = false;
        if (RegenerateRequested is not null)
        {
            await RegenerateRequested().ConfigureAwait(true);
        }
    }

    [RelayCommand]
    private void KeepEdits() => RegenerateWarningOpen = false;

    [RelayCommand(CanExecute = nameof(CanTranslate))]
    private Task Translate() => StartTranslation(SelectedLanguage!);

    /// <summary>Translates again into the translation's own language, whatever the picker shows.</summary>
    [RelayCommand(CanExecute = nameof(CanTranslateAgain))]
    private Task TranslateAgain() => StartTranslation(TranslationLanguage);

    private Task StartTranslation(string language)
    {
        TranslationRunning = true;
        return TranslateRequested!(language);
    }

    private bool CanTranslate() => SelectedLanguage is not null && CanStartTranslation();

    private bool CanTranslateAgain() =>
        TranslationStale && TranslationLanguage.Length > 0 && CanStartTranslation();

    // The engine translates the stored sheet, which exists only once the
    // pipeline reports it ready. Text alone streams in before that
    private bool CanStartTranslation() => TranslateRequested is not null
        && PipelineState == NotePipelineState.AllReady && !TranslationRunning && !AnyEditing;

    // An edited note is the clinician's wording. Regenerating replaces it,
    // so it asks first. An unedited note regenerates straight away
    [RelayCommand(CanExecute = nameof(CanRegenerate))]
    private Task Regenerate()
    {
        if (Edited)
        {
            RegenerateWarningOpen = true;
            return Task.CompletedTask;
        }

        return RegenerateRequested!();
    }

    // Any settled review state can regenerate, including a failed note,
    // since regenerating is the recovery. The engine refuses what it cannot do
    private bool CanRegenerate() => RegenerateRequested is not null && !AnyEditing
        && PipelineState is NotePipelineState.AllReady or NotePipelineState.PatientFailed
        or NotePipelineState.NoteFailed;

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

    [RelayCommand(CanExecute = nameof(CanEditPatient))]
    private void EditPatient()
    {
        _patientSnapshot = PatientInfoText;
        PatientEditing = true;
    }

    private bool CanEditPatient() => PatientDocumentReady && !PatientEditing;

    [RelayCommand]
    private void DiscardPatient()
    {
        PatientInfoText = _patientSnapshot;
        PatientEditing = false;
    }

    [RelayCommand(CanExecute = nameof(CanSaveNote))]
    private Task SaveNote()
    {
        NoteEditing = false;
        return SaveNoteRequested!();
    }

    private bool CanSaveNote() => SaveNoteRequested is not null && NoteDocumentReady;

    [RelayCommand(CanExecute = nameof(CanSavePatient))]
    private Task SavePatient()
    {
        PatientEditing = false;
        return SavePatientRequested!();
    }

    private bool CanSavePatient() => SavePatientRequested is not null && PatientDocumentReady;

    /// <summary>The pane returns to its writing look for a rewrite.</summary>
    public void BeginRegenerate() => ClearDocuments(NotePipelineState.NoteWriting);

    /// <summary>
    /// A stored session's documents, ready for review. The options show the
    /// stored values without becoming the clinician's new defaults.
    /// </summary>
    public void LoadStored(
        string note, string patient, string translation,
        string style, string detail, string editedStamp,
        string translationLanguage = "")
    {
        TranslationLanguage = translationLanguage;
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
        PatientEditing = false;
        ClinicalNoteText = note;
        PatientInfoText = patient;
        TranslationText = translation;
        TranslationRunning = false;
        TranslationStale = false;
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
        TranslateCommand.NotifyCanExecuteChanged();
        TranslateAgainCommand.NotifyCanExecuteChanged();
        RegenerateCommand.NotifyCanExecuteChanged();
        SaveNoteCommand.NotifyCanExecuteChanged();
        SavePatientCommand.NotifyCanExecuteChanged();
        EditNoteCommand.NotifyCanExecuteChanged();
        EditPatientCommand.NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(NoteDocumentReady));
        OnPropertyChanged(nameof(PatientDocumentReady));
        OnPropertyChanged(nameof(NotePreparing));
        OnPropertyChanged(nameof(PatientPreparing));
        OnPropertyChanged(nameof(NoteStateCaption));
        OnPropertyChanged(nameof(NoteRefused));
        OnPropertyChanged(nameof(CanWriteAnyway));
        WriteAnywayCommand.NotifyCanExecuteChanged();
        ReflectCommand.NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(PatientStateCaption));
        OnPropertyChanged(nameof(NoteCaptionVisible));
        OnPropertyChanged(nameof(PatientCaptionVisible));
    }

    partial void OnClinicalNoteTextChanged(string value) =>
        OnPropertyChanged(nameof(NotePreparing));

    partial void OnPatientInfoTextChanged(string value) =>
        OnPropertyChanged(nameof(PatientPreparing));

    partial void OnTranslationTextChanged(string value)
    {
        OnPropertyChanged(nameof(TranslationVisible));
        OnPropertyChanged(nameof(PatientCopyTip));
        OnPropertyChanged(nameof(PatientExportTip));
    }

    partial void OnTranslationLanguageChanged(string value) =>
        OnPropertyChanged(nameof(TranslationCaption));

    partial void OnExampleCaseIndexChanged(int value)
    {
        if (value == 0)
        {
            OriginalNoteRequested?.Invoke();
        }
        else if (value > 0 && value <= ExampleCases.Count)
        {
            ExampleCaseRequested?.Invoke(ExampleCases[value - 1]);
        }
    }

    partial void OnStyleChanged(string value) => OptionChanged();

    partial void OnDetailChanged(string value) => OptionChanged();

    partial void OnTranslationRunningChanged(bool value) =>
        OnPropertyChanged(nameof(TranslationVisible));

    partial void OnEditedStampChanged(string value) => OnPropertyChanged(nameof(Edited));

    partial void OnPatientStaleChanged(bool value) =>
        RegeneratePatientCommand.NotifyCanExecuteChanged();

    partial void OnNoteEditingChanged(bool value) => EditingChanged();

    partial void OnPatientEditingChanged(bool value) => EditingChanged();

    // Regenerate, the sheet rewrite and translate all replace on-screen text,
    // so none may run while a document is being edited
    private void EditingChanged()
    {
        OnPropertyChanged(nameof(NoteViewing));
        OnPropertyChanged(nameof(PatientViewing));
        OnPropertyChanged(nameof(AnyEditing));
        EditNoteCommand.NotifyCanExecuteChanged();
        EditPatientCommand.NotifyCanExecuteChanged();
        RegenerateCommand.NotifyCanExecuteChanged();
        RegeneratePatientCommand.NotifyCanExecuteChanged();
        TranslateCommand.NotifyCanExecuteChanged();
        TranslateAgainCommand.NotifyCanExecuteChanged();
    }

    // Clears every document and stamp, an open edit included, since the next text replaces them
    private void ClearDocuments(NotePipelineState state)
    {
        NoteEditing = false;
        PatientEditing = false;
        RefusalReason = "";
        PipelineState = state;
        ClinicalNoteText = "";
        PatientInfoText = "";
        TranslationText = "";
        TranslationLanguage = "";
        TranslationRunning = false;
        EditedStamp = "";
        PatientStale = false;
        TranslationStale = false;
        ExampleCaseIndex = -1;
    }

    private void OptionChanged()
    {
        if (!_suppressOptionsChanged)
        {
            OptionsChanged?.Invoke();
        }
    }
}
