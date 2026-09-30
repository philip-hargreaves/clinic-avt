using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace ClinicAVT.App.Core.Features.Documents;

public sealed partial class ReviewCommandsViewModel : ObservableObject
{
    private static readonly HashSet<string?> Gates =
    [
        nameof(NoteViewModel.PipelineState), nameof(NoteViewModel.NoteEditing),
        nameof(NoteViewModel.WriteAnywayAvailable), nameof(NoteViewModel.ReflectAvailable),
        nameof(PatientSheetViewModel.PatientEditing), nameof(PatientSheetViewModel.PatientStale),
        nameof(PatientSheetViewModel.SelectedLanguage), nameof(PatientSheetViewModel.TranslationLanguage),
        nameof(PatientSheetViewModel.TranslationRunning), nameof(PatientSheetViewModel.TranslationStale),
    ];

    private readonly NoteViewModel _note;
    private readonly PatientSheetViewModel _patient;
    private readonly IReviewActions _actions;

    public ReviewCommandsViewModel(NoteViewModel note, PatientSheetViewModel patient, IReviewActions actions)
    {
        _note = note;
        _patient = patient;
        _actions = actions;
        note.PropertyChanged += OnDocumentChanged;
        patient.PropertyChanged += OnDocumentChanged;
    }

    // Regenerate, the sheet rewrite and translate all replace on-screen text,
    // so none may run while a document is being edited
    private bool AnyEditing => _note.NoteEditing || _patient.PatientEditing;

    /// <summary>
    /// True while a document is being edited, or a note, sheet or translation is still being written.
    /// </summary>
    public bool Busy => AnyEditing || _patient.TranslationRunning
        || _note.PipelineState is NotePipelineState.Pending or NotePipelineState.NoteWriting
            or NotePipelineState.NoteReadyPatientWriting;

    [RelayCommand(CanExecute = nameof(CanWriteAnyway))]
    private Task WriteAnyway() => _actions.WriteNoteAnywayAsync();

    private bool CanWriteAnyway() => _note.CanWriteAnyway;

    [RelayCommand(CanExecute = nameof(CanReflect))]
    private Task Reflect() => _actions.ReflectAsync();

    private bool CanReflect() => _note.ReflectAvailable && _note.NoteDocumentReady;

    /// <summary>
    /// The staleness hint's action, which rewrites the sheet from the edited note.
    /// </summary>
    [RelayCommand(CanExecute = nameof(CanRegeneratePatient))]
    private Task RegeneratePatient() => _actions.RegeneratePatientAsync();

    private bool CanRegeneratePatient() => _patient.PatientStale && !AnyEditing;

    [RelayCommand]
    private async Task ConfirmRegenerate()
    {
        _note.RegenerateWarningOpen = false;
        await _actions.RegenerateNoteAsync().ConfigureAwait(true);
    }

    // An edited note is the clinician's wording, so confirm before regenerating over it
    [RelayCommand(CanExecute = nameof(CanRegenerate))]
    private Task Regenerate()
    {
        if (_note.Edited)
        {
            _note.RegenerateWarningOpen = true;
            return Task.CompletedTask;
        }

        return _actions.RegenerateNoteAsync();
    }

    // Any settled review state can regenerate, including a failed note,
    // since regenerating is the recovery. The engine refuses what it cannot do
    private bool CanRegenerate() => !AnyEditing
        && _note.PipelineState is NotePipelineState.AllReady or NotePipelineState.PatientFailed
        or NotePipelineState.NoteFailed;

    [RelayCommand(CanExecute = nameof(CanTranslate))]
    private Task Translate() => StartTranslation(_patient.SelectedLanguage!);

    /// <summary>Translates again into the translation's own language, whatever the picker shows.</summary>
    [RelayCommand(CanExecute = nameof(CanTranslateAgain))]
    private Task TranslateAgain() => StartTranslation(_patient.TranslationLanguage);

    private Task StartTranslation(string language)
    {
        _patient.TranslationRunning = true;
        return _actions.TranslateAsync(language);
    }

    private bool CanTranslate() => _patient.SelectedLanguage is not null && CanStartTranslation();

    private bool CanTranslateAgain() =>
        _patient.TranslationStale && _patient.TranslationLanguage.Length > 0 && CanStartTranslation();

    // The engine translates the stored sheet, which exists only once the
    // pipeline reports it ready. Text alone streams in before that
    private bool CanStartTranslation() =>
        _note.PipelineState == NotePipelineState.AllReady && !_patient.TranslationRunning && !AnyEditing;

    [RelayCommand(CanExecute = nameof(CanSaveNote))]
    private Task SaveNote()
    {
        _note.FinishEditing();
        return _actions.SaveNoteAsync();
    }

    private bool CanSaveNote() => _note.NoteDocumentReady;

    [RelayCommand(CanExecute = nameof(CanSavePatient))]
    private Task SavePatient()
    {
        _patient.FinishEditing();
        return _actions.SavePatientAsync();
    }

    private bool CanSavePatient() => _patient.PatientDocumentReady;

    private void OnDocumentChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (!Gates.Contains(e.PropertyName))
        {
            return;
        }

        WriteAnywayCommand.NotifyCanExecuteChanged();
        ReflectCommand.NotifyCanExecuteChanged();
        RegeneratePatientCommand.NotifyCanExecuteChanged();
        RegenerateCommand.NotifyCanExecuteChanged();
        TranslateCommand.NotifyCanExecuteChanged();
        TranslateAgainCommand.NotifyCanExecuteChanged();
        SaveNoteCommand.NotifyCanExecuteChanged();
        SavePatientCommand.NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(Busy));
    }
}
