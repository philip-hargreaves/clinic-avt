using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Features.Documents;

public class DocumentExportViewModelTest
{
    private static (DocumentExportViewModel Export, NoteViewModel Note, FakeClipboard Clipboard,
        FakeFilePicker Picker, StatusLine Status, TestShell Shell) Create()
    {
        var shell = new TestShell();
        return (shell.Get<DocumentExportViewModel>(), shell.Note, shell.Clipboard, shell.Picker, shell.Line, shell);
    }

    // The patient takes the sheet home in their language, and the record shows what they were given
    [Fact]
    public async Task CopyPutsTheDocumentOnTheClipboardWithItsTranslationOnceThereIsOne()
    {
        var (export, note, clipboard, _, status, shell) = Create();
        note.ClinicalNoteText = "the note";
        shell.Patient.PatientInfoText = "take\rone tablet";  // as an edit box leaves it
        Assert.Equal("Copies the patient information", shell.Patient.PatientCopyTip);

        await export.CopyNoteCommand.ExecuteAsync(null);
        await export.CopyPatientCommand.ExecuteAsync(null);
        Assert.Equal(["the note", "take\r\none tablet"], clipboard.Copied);
        Assert.Contains("Patient information copied", status.LatestActivity);

        shell.Patient.TranslationLanguage = "Urdu";
        shell.Patient.TranslationText = "ایک گولی لیں";
        Assert.Equal("Copies the patient information and its translation", shell.Patient.PatientCopyTip);
        Assert.Contains("translation", shell.Patient.PatientExportTip);

        await export.CopyPatientCommand.ExecuteAsync(null);
        Assert.Equal("take\r\none tablet\r\n\r\nUrdu translation\r\n\r\nایک گولی لیں", clipboard.Copied[^1]);
    }

    // Arabic-script translations read right to left, so their box flows that way
    [Fact]
    public void RightToLeftLanguagesFlowRightToLeft()
    {
        var shell = new TestShell();
        var note = shell.Note;
        foreach (var language in new[] { "Arabic", "Farsi", "Kurdish (Sorani)", "Urdu" })
        {
            shell.Patient.TranslationLanguage = language;
            Assert.True(shell.Patient.TranslationRightToLeft, language);
        }

        foreach (var language in new[] { "Kurdish (Kurmanji)", "Greek", "Chinese (Simplified)", "" })
        {
            shell.Patient.TranslationLanguage = language;
            Assert.False(shell.Patient.TranslationRightToLeft, language);
        }
    }

    [Fact]
    public async Task ExportWritesTheFileWithTheMarkerAndTheTranslationAndACancelledPickerWritesNothing()
    {
        var (export, note, _, picker, status, shell) = Create();
        var files = (FakeTextFiles)shell.Get<ITextFiles>();
        note.ClinicalNoteText = "the note";
        shell.Patient.PatientInfoText = "take\rone tablet";
        shell.Patient.TranslationText = "prendre un comprimé";
        shell.Patient.TranslationLanguage = "French";

        picker.SavePath = null;
        await export.ExportNoteCommand.ExecuteAsync(null);
        Assert.Equal(["clinical-note.txt"], picker.SuggestedNames);
        Assert.Equal("", status.LatestActivity);
        Assert.Empty(files.Written);

        picker.SavePath = @"C:\Users\clinician\Documents\sheet.txt";
        await export.ExportPatientCommand.ExecuteAsync(null);

        var written = files.Written[picker.SavePath];
        Assert.StartsWith(DocumentExport.Marker, written);
        Assert.Contains("take\none tablet\n\nFrench translation\n\nprendre un comprimé", written);
        Assert.Equal(["clinical-note.txt", "patient-sheet.txt"], picker.SuggestedNames);
        Assert.Contains("outside the encrypted store", status.LatestActivity);
    }
}
