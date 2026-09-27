using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Shell;

namespace ClinicAVT.App.Core.Features.Documents;

/// <summary>Copy and Export for the note and the patient sheet.</summary>
public sealed partial class DocumentExportViewModel(
    NoteViewModel note, IClipboard clipboard, IFilePicker picker, StatusBarViewModel status)
    : ObservableObject
{
    [RelayCommand]
    private Task CopyNote() => clipboard.CopyAsync(status, note.ClinicalNoteText, "Note");

    [RelayCommand]
    private Task ExportNote() => ExportAsync("clinical-note.txt", note.ClinicalNoteText);

    [RelayCommand]
    private Task CopyPatient() => clipboard.CopyAsync(status, PatientSheet(), "Patient note");

    [RelayCommand]
    private Task ExportPatient() => ExportAsync("patient-sheet.txt", PatientSheet());

    // The sheet and its translation travel together to the patient
    private string PatientSheet()
    {
        var text = note.PatientInfoText;
        if (note.TranslationText.Length > 0)
        {
            text += "\n\n" + note.TranslationCaption + "\n\n" + note.TranslationText;
        }

        return text;
    }

    // Export is the one action that writes outside the encrypted store
    private async Task ExportAsync(string suggestedName, string text)
    {
        if (await picker.SaveTextAsync(suggestedName, "Text file", ".txt",
                DocumentExport.Marker + text.ReplaceLineEndings("\n"))
                .ConfigureAwait(true) is { } path)
        {
            status.Append($"Saved to {Path.GetFileName(path)} - outside the encrypted store");
        }
    }
}
