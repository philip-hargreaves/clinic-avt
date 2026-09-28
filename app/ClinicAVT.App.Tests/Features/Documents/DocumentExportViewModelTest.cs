using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Features.Documents;

public class DocumentExportViewModelTest
{
    private static (DocumentExportViewModel Export, NoteViewModel Note, FakeClipboard Clipboard,
        FakeFilePicker Picker, StatusBarViewModel Status) Create()
    {
        var note = new NoteViewModel();
        var clipboard = new FakeClipboard();
        var picker = new FakeFilePicker();
        var status = TestSession.Status();
        return (new DocumentExportViewModel(note, clipboard, picker, status), note, clipboard, picker, status);
    }

    // The patient takes the sheet home in their language, and the record shows what they were given
    [Fact]
    public async Task CopyPutsTheDocumentOnTheClipboardWithItsTranslationOnceThereIsOne()
    {
        var (export, note, clipboard, _, status) = Create();
        note.ClinicalNoteText = "the note";
        note.PatientInfoText = "take\rone tablet";  // as an edit box leaves it
        Assert.Equal("Copies the patient information", note.PatientCopyTip);

        await export.CopyNoteCommand.ExecuteAsync(null);
        await export.CopyPatientCommand.ExecuteAsync(null);
        Assert.Equal(["the note", "take\r\none tablet"], clipboard.Copied);
        Assert.Contains("Patient information copied", status.LatestActivity);

        note.TranslationLanguage = "Urdu";
        note.TranslationText = "ایک گولی لیں";
        Assert.Equal("Copies the patient information and its translation", note.PatientCopyTip);
        Assert.Contains("translation", note.PatientExportTip);

        await export.CopyPatientCommand.ExecuteAsync(null);
        Assert.Equal("take\r\none tablet\r\n\r\nUrdu translation\r\n\r\nایک گولی لیں", clipboard.Copied[^1]);
    }

    // Arabic-script translations read right to left, so their box flows that way
    [Fact]
    public void RightToLeftLanguagesFlowRightToLeft()
    {
        var note = new NoteViewModel();
        foreach (var language in new[] { "Arabic", "Farsi", "Kurdish (Sorani)", "Urdu" })
        {
            note.TranslationLanguage = language;
            Assert.True(note.TranslationRightToLeft, language);
        }

        foreach (var language in new[] { "Kurdish (Kurmanji)", "Greek", "Chinese (Simplified)", "" })
        {
            note.TranslationLanguage = language;
            Assert.False(note.TranslationRightToLeft, language);
        }
    }

    [Fact]
    public async Task ExportWritesTheFileWithTheMarkerAndTheTranslationAndACancelledPickerWritesNothing()
    {
        var (export, note, _, picker, status) = Create();
        var dir = Directory.CreateTempSubdirectory();
        try
        {
            note.ClinicalNoteText = "the note";
            note.PatientInfoText = "take\rone tablet";
            note.TranslationText = "prendre un comprimé";
            note.TranslationLanguage = "French";

            picker.SavePath = null;
            await export.ExportNoteCommand.ExecuteAsync(null);
            Assert.Equal(["clinical-note.txt"], picker.SuggestedNames);
            Assert.Equal("", status.LatestActivity);
            Assert.Empty(Directory.GetFiles(dir.FullName));

            picker.SavePath = Path.Combine(dir.FullName, "sheet.txt");
            await export.ExportPatientCommand.ExecuteAsync(null);

            var written = await File.ReadAllTextAsync(picker.SavePath);
            Assert.StartsWith(DocumentExport.Marker, written);
            Assert.Contains("take\none tablet\n\nFrench translation\n\nprendre un comprimé", written);
            Assert.Equal(["clinical-note.txt", "patient-sheet.txt"], picker.SuggestedNames);
            Assert.Contains("outside the encrypted store", status.LatestActivity);
        }
        finally
        {
            dir.Delete(recursive: true);
        }
    }
}
