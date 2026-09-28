using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.Support;

namespace ClinicAVT.App.Tests.Features.Consultation;

/// <summary>A stored session opened for review through the consultation VM.</summary>
public class ReviewSessionTest
{
    [Fact]
    public async Task TheStoredNoteIsReadOnlyUntilEditNothingRewritesWhileEditingAndRegenerateAsksOnlyWhenEdited()
    {
        var (session, _, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        note.ClinicalNoteText = "the stored note";
        var saves = 0;
        note.SaveNoteRequested = () =>
        {
            saves++;
            return Task.CompletedTask;
        };
        var regenerates = 0;
        note.RegenerateRequested = () =>
        {
            regenerates++;
            return Task.CompletedTask;
        };
        note.RegeneratePatientRequested = () => Task.CompletedTask;
        note.PatientStale = true;
        note.SelectedLanguage = "French";

        Assert.True(note.NoteViewing, "read-only until an explicit Edit");
        Assert.True(note.RegenerateCommand.CanExecute(null));
        note.EditNoteCommand.Execute(null);
        Assert.True(note.NoteEditing);
        Assert.False(note.RegenerateCommand.CanExecute(null));
        Assert.False(note.RegeneratePatientCommand.CanExecute(null));
        Assert.False(note.TranslateCommand.CanExecute(null));

        note.ClinicalNoteText = "a stray keystroke";
        note.DiscardNoteCommand.Execute(null);
        Assert.Equal("the stored note", note.ClinicalNoteText);
        Assert.True(note.NoteViewing);
        Assert.Equal(0, saves);
        Assert.True(note.RegenerateCommand.CanExecute(null));

        note.EditNoteCommand.Execute(null);
        note.ClinicalNoteText = "a deliberate edit";
        await note.SaveNoteCommand.ExecuteAsync(null);
        Assert.Equal(1, saves);
        Assert.True(note.NoteViewing);
        Assert.Equal("a deliberate edit", note.ClinicalNoteText);

        note.EditedStamp = "";
        await note.RegenerateCommand.ExecuteAsync(null);
        Assert.False(note.RegenerateWarningOpen, "an unedited note regenerates without asking");
        Assert.Equal(1, regenerates);

        note.EditedStamp = "Edited 10:31";
        await note.RegenerateCommand.ExecuteAsync(null);
        Assert.True(note.RegenerateWarningOpen, "an edited note warns");
        Assert.Equal(1, regenerates);

        note.KeepEditsCommand.Execute(null);
        Assert.False(note.RegenerateWarningOpen);
        Assert.Equal(1, regenerates);

        await note.RegenerateCommand.ExecuteAsync(null);
        await note.ConfirmRegenerateCommand.ExecuteAsync(null);
        Assert.False(note.RegenerateWarningOpen);
        Assert.Equal(2, regenerates);
        Assert.True(note.Edited);

        note.BeginRegenerate();
        Assert.False(note.Edited);
    }

    [Fact]
    public async Task SavingANoteEditMarksTheSheetStaleAndRegeneratingItRewritesFromTheStoredNote()
    {
        var (session, engine, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        note.PatientInfoText = "old sheet";
        Assert.False(note.PatientStale);
        Assert.False(note.RegeneratePatientCommand.CanExecute(null));

        note.ClinicalNoteText = "corrected";
        await session.SaveNoteAsync();
        Assert.True(note.PatientStale, "the sheet no longer derives from the note");
        Assert.True(note.RegeneratePatientCommand.CanExecute(null));

        await note.RegeneratePatientCommand.ExecuteAsync(null);

        Assert.Contains(engine.Requests, r => r.Method == "patient/regenerate");
        engine.RaiseNotification("patient/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "fresh sheet" }));
        Assert.Equal("fresh sheet", note.PatientInfoText);
        Assert.False(note.PatientStale, "rewritten from the note, no longer stale");
        Assert.False(note.RegeneratePatientCommand.CanExecute(null));
    }

    [Fact]
    public async Task EditingTheTranslatedSheetMarksTheTranslationStaleAndTranslateAgainKeepsItsLanguage()
    {
        var (session, engine, note) = TestSession.Create();
        engine.StoredPatient = "Take one tablet a day.";
        await session.OpenStoredSessionAsync("abc");
        engine.RaiseNotification("translate/ready", Translation("Jedna tabletka dziennie.", "Polish"));
        Assert.False(note.TranslationStale);
        Assert.False(note.TranslateAgainCommand.CanExecute(null));

        Assert.True(note.PatientViewing, "the sheet is read until an explicit Edit");
        note.EditPatientCommand.Execute(null);
        Assert.True(note.PatientEditing);
        note.PatientInfoText = "Take two tablets a day.";
        Assert.False(note.TranslationStale, "an unsaved edit changes nothing yet");
        Assert.False(note.TranslateAgainCommand.CanExecute(null), "nothing is translated mid-edit");

        await note.SavePatientCommand.ExecuteAsync(null);
        Assert.True(note.PatientViewing, "saving returns to reading");
        Assert.True(note.TranslationStale, "the translation says one tablet, the sheet two");

        note.SelectedLanguage = "French";
        await note.TranslateAgainCommand.ExecuteAsync(null);
        var request = engine.Requests.Last(r => r.Method == "patient/translate");
        Assert.Contains("Polish", request.Params);
        Assert.DoesNotContain("French", request.Params);

        engine.RaiseNotification("translate/ready", Translation("Dwie tabletki dziennie.", "Polish"));
        Assert.False(note.TranslationStale, "the new translation matches the sheet");
        Assert.False(note.TranslateAgainCommand.CanExecute(null));
    }

    [Fact]
    public async Task ARewrittenSheetMarksTheTranslationStaleButAnUnchangedSaveDoesNot()
    {
        var (session, engine, note) = TestSession.Create();
        engine.StoredPatient = "Take one tablet a day.";
        await session.OpenStoredSessionAsync("abc");
        engine.RaiseNotification("translate/ready", Translation("Jedna tabletka dziennie.", "Polish"));

        note.EditPatientCommand.Execute(null);
        await note.SavePatientCommand.ExecuteAsync(null);
        Assert.False(note.TranslationStale, "the sheet did not change");

        engine.RaiseNotification("patient/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "A rewritten sheet." }));
        Assert.True(note.TranslationStale);

        await session.CloseReviewAsync();
        Assert.False(note.TranslationStale, "leaving clears it");
    }

    [Fact]
    public async Task AStoredSheetOlderThanTheNoteEditLoadsStale()
    {
        var (session, engine, note) = TestSession.Create();
        engine.StoredNote = "text";
        engine.StoredNoteEditedAt = "2026-08-17T10:31:00Z";
        engine.StoredPatient = "sheet";
        engine.StoredPatientGeneratedAt = "2026-08-17T10:24:00Z";

        Assert.True(await session.OpenStoredSessionAsync("abc"));
        Assert.True(note.PatientStale, "the sheet predates the note edit");

        engine.StoredPatientGeneratedAt = "2026-08-17T10:32:00Z";
        await session.CloseReviewAsync();
        Assert.False(note.PatientStale, "leaving clears it");
        Assert.True(await session.OpenStoredSessionAsync("abc"));
        Assert.False(note.PatientStale, "a sheet rewritten after the edit is current");
    }

    [Fact]
    public async Task AStoredTranslationOlderThanTheSheetsLastChangeLoadsStale()
    {
        var (session, engine, note) = TestSession.Create();
        engine.StoredPatient = "Take two tablets a day.";
        engine.StoredPatientGeneratedAt = "2026-08-17T10:24:00Z";
        engine.StoredTranslation = "Jedna tabletka dziennie.";
        engine.StoredTranslatedAt = "2026-08-17T10:26:00Z";
        engine.StoredPatientEditedAt = "2026-08-17T10:31:00Z";

        Assert.True(await session.OpenStoredSessionAsync("abc"));
        Assert.Equal("Jedna tabletka dziennie.", note.TranslationText);
        Assert.True(note.TranslationStale, "the sheet was edited after it was translated");
        Assert.True(note.TranslateAgainCommand.CanExecute(null));

        engine.StoredPatientEditedAt = null;
        await session.CloseReviewAsync();
        Assert.True(await session.OpenStoredSessionAsync("abc"));
        Assert.False(note.TranslationStale, "translated after the sheet was written");

        engine.StoredPatientGeneratedAt = "2026-08-17T10:40:00Z";
        await session.CloseReviewAsync();
        Assert.True(await session.OpenStoredSessionAsync("abc"));
        Assert.True(note.TranslationStale, "the sheet was rewritten after it was translated");
    }

    [Fact]
    public async Task OpeningAnotherStartingAnewOrClosingMidEditAllSaveTheEdits()
    {
        var (session, engine, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        Assert.Equal(SessionState.Review, session.State);
        note.ClinicalNoteText = "edited in review";

        await session.OpenStoredSessionAsync("def");

        Assert.Contains(engine.Requests, r => r.Method == "note/update"
            && r.Params.Contains("abc") && r.Params.Contains("edited in review"));
        Assert.Contains(engine.Requests, r => r.Method == "session/open"
            && r.Params.Contains("def"));

        note.ClinicalNoteText = "corrected wording";
        session.FinishConsultation();

        Assert.Contains(engine.Requests, r => r.Method == "note/update"
            && r.Params.Contains("corrected wording"));
        Assert.Contains(engine.Requests, r => r.Method == "session/close");
        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal("", note.ClinicalNoteText);
        Assert.Equal(NotePipelineState.Pending, note.PipelineState);

        await session.OpenStoredSessionAsync("abc");
        note.EditNoteCommand.Execute(null);
        note.ClinicalNoteText = "edited then left";
        await session.CloseReviewAsync();

        Assert.Contains(engine.Requests,
            r => r.Method == "note/update" && r.Params.Contains("edited then left"));
    }

    [Fact]
    public void LoadStoredShowsTheOptionsWithoutMakingThemDefaults()
    {
        var note = new NoteViewModel();
        var optionChanges = 0;
        note.OptionsChanged = () => optionChanges++;

        note.LoadStored("text", "sheet", "", "soap", "concise", "Edited 10:31");

        Assert.Equal("soap", note.Style);
        Assert.Equal("concise", note.Detail);
        Assert.Equal(0, optionChanges);
        Assert.Equal(NotePipelineState.AllReady, note.PipelineState);
        Assert.True(note.Edited);

        note.Style = "prose";  // the clinician's own change still registers
        Assert.Equal(1, optionChanges);
    }

    [Fact]
    public async Task StartCarriesTheKeepConsultationsSetting()
    {
        var preferences = new AppPreferences(
            Path.Combine(Path.GetTempPath(), Path.GetRandomFileName()));
        var (session, engine, _) = TestSession.Create(preferences);

        await session.StartRecordingAsync();
        Assert.Contains(engine.Requests, r => r.Method == "session/start"
            && r.Params.Contains("\"retain\":false"));
        await session.StopRecordingAsync();

        preferences.KeepConsultations = true;
        engine.RaiseNotification("note/ready");
        session.FinishConsultation();
        await session.StartRecordingAsync();
        Assert.Contains(engine.Requests, r => r.Method == "session/start"
            && r.Params.Contains("\"retain\":true"));
    }

    [Fact]
    public async Task OpeningIsRefusedWhileRecording()
    {
        var (session, engine, _) = TestSession.Create();
        await session.StartRecordingAsync();

        Assert.False(await session.OpenStoredSessionAsync("abc"));
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/open");
    }

    private static System.Text.Json.JsonElement Translation(string text, string language) =>
        System.Text.Json.JsonSerializer.SerializeToElement(new { text, language });
}
