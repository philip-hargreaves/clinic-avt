using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Features.Documents;

public sealed partial class PatientSheetViewModel : ObservableObject
{
    // By the names the translation model lists
    private static readonly HashSet<string> RightToLeftLanguages = ["Arabic", "Farsi", "Kurdish (Sorani)", "Urdu"];

    private readonly NoteViewModel _note;
    private string _patientSnapshot = "";

    public PatientSheetViewModel(NoteViewModel note, IEngineEvents events)
    {
        _note = note;
        note.Cleared += Clear;
        note.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(NoteViewModel.PipelineState))
            {
                EditPatientCommand.NotifyCanExecuteChanged();
                OnPropertyChanged(nameof(PatientDocumentReady));
                OnPropertyChanged(nameof(PatientPreparing));
                OnPropertyChanged(nameof(PatientStateCaption));
                OnPropertyChanged(nameof(PatientCaptionVisible));
            }
        };
        // A translation running when the engine drops never finishes
        events.SubscribeConnection(connected =>
        {
            if (!connected)
            {
                TranslationRunning = false;
            }
        });
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PatientPreparing))]
    public partial string PatientInfoText { get; set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(TranslationVisible), nameof(PatientCopyTip), nameof(PatientExportTip))]
    public partial string TranslationText { get; set; } = "";

    /// <summary>The translation's language, such as "Polish". It heads the output box.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(TranslationRightToLeft), nameof(TranslationCaption))]
    public partial string TranslationLanguage { get; set; } = "";

    public bool TranslationRightToLeft => RightToLeftLanguages.Contains(TranslationLanguage);

    [ObservableProperty]
    public partial string? SelectedLanguage { get; set; }

    /// <summary>True from the request until translate/ready or translate/failed.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(TranslationVisible))]
    public partial bool TranslationRunning { get; set; }

    /// <summary>
    /// True when the note was edited after the sheet was written from it. It clears when the sheet
    /// is rewritten.
    /// </summary>
    [ObservableProperty]
    public partial bool PatientStale { get; set; }

    /// <summary>The sheet changed after it was translated. Cleared by a new translation.</summary>
    [ObservableProperty]
    public partial bool TranslationStale { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PatientViewing))]
    [NotifyCanExecuteChangedFor(nameof(EditPatientCommand))]
    public partial bool PatientEditing { get; private set; }

    public ObservableCollection<string> Languages { get; } = [];

    public string TranslationCaption =>
        TranslationLanguage.Length > 0 ? $"{TranslationLanguage} translation" : "Translation";

    public bool PatientViewing => !PatientEditing;

    public bool PatientDocumentReady => _note.PipelineState == NotePipelineState.AllReady;

    public bool PatientPreparing =>
        _note.PipelineState is NotePipelineState.NoteWriting or NotePipelineState.NoteReadyPatientWriting
        && PatientInfoText.Length == 0;

    public string PatientStateCaption => _note.PipelineState switch
    {
        NotePipelineState.NoteWriting or NotePipelineState.NoteReadyPatientWriting =>
            "Patient information follows the note",
        NotePipelineState.PatientFailed =>
            "Patient information could not be written - see the status bar",
        _ => "",
    };

    public bool PatientCaptionVisible => PatientStateCaption.Length > 0;

    public bool TranslationVisible => TranslationRunning || TranslationText.Length > 0;

    public string PatientCopyTip =>
        TranslationText.Length > 0 ? "Copies the patient information and its translation" : "Copies the patient information";

    public string PatientExportTip =>
        TranslationText.Length > 0 ? "Saves the patient information and its translation as a text file" : "Saves the patient information as a text file";

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

    public void FinishEditing() => PatientEditing = false;

    public void LoadStored(string patient, string translation, string translationLanguage)
    {
        TranslationLanguage = translationLanguage;
        PatientEditing = false;
        PatientInfoText = patient;
        TranslationText = translation;
        TranslationRunning = false;
        TranslationStale = false;
    }

    // Clears the sheet and its translation, an open edit included, since the next text replaces them
    private void Clear()
    {
        PatientEditing = false;
        PatientInfoText = "";
        TranslationText = "";
        TranslationLanguage = "";
        TranslationRunning = false;
        PatientStale = false;
        TranslationStale = false;
    }
}
