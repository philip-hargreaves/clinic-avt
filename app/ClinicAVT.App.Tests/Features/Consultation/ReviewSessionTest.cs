using Microsoft.Extensions.DependencyInjection;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Features.Consultation;

public class ReviewSessionTest
{
    [Fact]
    public async Task TheStoredNoteIsReadOnlyUntilEditNothingRewritesWhileEditingAndRegenerateAsksOnlyWhenEdited()
    {
        var actions = new CountingReviewActions();
        var shell = new TestShell(configure: services => services.AddSingleton<IReviewActions>(actions));
        var (session, _, note) = shell;
        await session.OpenStoredSessionAsync("abc");
        note.ClinicalNoteText = "the stored note";
        shell.Patient.PatientStale = true;
        shell.Patient.SelectedLanguage = "French";

        Assert.True(note.NoteViewing, "read-only until an explicit Edit");
        Assert.True(shell.Commands.RegenerateCommand.CanExecute(null));
        note.EditNoteCommand.Execute(null);
        Assert.True(note.NoteEditing);
        Assert.False(shell.Commands.RegenerateCommand.CanExecute(null));
        Assert.False(shell.Commands.RegeneratePatientCommand.CanExecute(null));
        Assert.False(shell.Commands.TranslateCommand.CanExecute(null));

        note.ClinicalNoteText = "a stray keystroke";
        note.DiscardNoteCommand.Execute(null);
        Assert.Equal("the stored note", note.ClinicalNoteText);
        Assert.True(note.NoteViewing);
        Assert.Equal(0, actions.Saves);
        Assert.True(shell.Commands.RegenerateCommand.CanExecute(null));

        note.EditNoteCommand.Execute(null);
        note.ClinicalNoteText = "a deliberate edit";
        await shell.Commands.SaveNoteCommand.ExecuteAsync(null);
        Assert.Equal(1, actions.Saves);
        Assert.True(note.NoteViewing);
        Assert.Equal("a deliberate edit", note.ClinicalNoteText);

        note.EditedStamp = "";
        await shell.Commands.RegenerateCommand.ExecuteAsync(null);
        Assert.False(note.RegenerateWarningOpen, "an unedited note regenerates without asking");
        Assert.Equal(1, actions.Regenerates);

        note.EditedStamp = "Edited 10:31";
        await shell.Commands.RegenerateCommand.ExecuteAsync(null);
        Assert.True(note.RegenerateWarningOpen, "an edited note warns");
        Assert.Equal(1, actions.Regenerates);

        note.KeepEditsCommand.Execute(null);
        Assert.False(note.RegenerateWarningOpen);
        Assert.Equal(1, actions.Regenerates);

        await shell.Commands.RegenerateCommand.ExecuteAsync(null);
        await shell.Commands.ConfirmRegenerateCommand.ExecuteAsync(null);
        Assert.False(note.RegenerateWarningOpen);
        Assert.Equal(2, actions.Regenerates);
        Assert.True(note.Edited);

        note.BeginRegenerate();
        Assert.False(note.Edited);
    }

    [Fact]
    public async Task SavingANoteEditMarksTheSheetStaleAndRegeneratingItRewritesFromTheStoredNote()
    {
        var shell = TestSession.Create();
        var (session, engine, note) = shell;
        await session.OpenStoredSessionAsync("abc");
        shell.Patient.PatientInfoText = "old sheet";
        Assert.False(shell.Patient.PatientStale);
        Assert.False(shell.Commands.RegeneratePatientCommand.CanExecute(null));

        note.ClinicalNoteText = "corrected";
        await shell.Get<DocumentActions>().SaveNoteAsync();
        Assert.True(shell.Patient.PatientStale, "the sheet no longer derives from the note");
        Assert.True(shell.Commands.RegeneratePatientCommand.CanExecute(null));

        await shell.Commands.RegeneratePatientCommand.ExecuteAsync(null);

        Assert.Contains(engine.Requests, r => r.Method == "patient/regenerate");
        engine.RaiseNotification("patient/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "fresh sheet" }));
        Assert.Equal("fresh sheet", shell.Patient.PatientInfoText);
        Assert.False(shell.Patient.PatientStale, "rewritten from the note, no longer stale");
        Assert.False(shell.Commands.RegeneratePatientCommand.CanExecute(null));
    }

    [Fact]
    public async Task ChangingTheTranslatedSheetMarksTheTranslationStaleAndTranslateAgainKeepsItsLanguage()
    {
        var shell = TestSession.Create();
        var (session, engine, note) = shell;
        engine.StoredPatient = "Take one tablet a day.";
        await session.OpenStoredSessionAsync("abc");
        engine.RaiseNotification("translate/ready", Translation("Jedna tabletka dziennie.", "Polish"));
        Assert.False(shell.Patient.TranslationStale);
        Assert.False(shell.Commands.TranslateAgainCommand.CanExecute(null));

        shell.Patient.EditPatientCommand.Execute(null);
        await shell.Commands.SavePatientCommand.ExecuteAsync(null);
        Assert.False(shell.Patient.TranslationStale, "the sheet did not change");

        Assert.True(shell.Patient.PatientViewing, "the sheet is read until an explicit Edit");
        shell.Patient.EditPatientCommand.Execute(null);
        Assert.True(shell.Patient.PatientEditing);
        shell.Patient.PatientInfoText = "Take two tablets a day.";
        Assert.False(shell.Patient.TranslationStale, "an unsaved edit changes nothing yet");
        Assert.False(shell.Commands.TranslateAgainCommand.CanExecute(null), "nothing is translated mid-edit");

        await shell.Commands.SavePatientCommand.ExecuteAsync(null);
        Assert.True(shell.Patient.PatientViewing, "saving returns to reading");
        Assert.True(shell.Patient.TranslationStale, "the translation says one tablet, the sheet two");

        shell.Patient.SelectedLanguage = "French";
        await shell.Commands.TranslateAgainCommand.ExecuteAsync(null);
        var request = engine.Requests.Last(r => r.Method == "patient/translate");
        Assert.Contains("Polish", request.Params);
        Assert.DoesNotContain("French", request.Params);

        engine.RaiseNotification("translate/ready", Translation("Dwie tabletki dziennie.", "Polish"));
        Assert.False(shell.Patient.TranslationStale, "the new translation matches the sheet");
        Assert.False(shell.Commands.TranslateAgainCommand.CanExecute(null));

        engine.RaiseNotification("patient/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "A rewritten sheet." }));
        Assert.True(shell.Patient.TranslationStale);

        await session.CloseReviewAsync();
        Assert.False(shell.Patient.TranslationStale, "leaving clears it");
    }

    [Fact]
    public async Task AStoredSheetOrTranslationOlderThanWhatItDerivesFromLoadsStale()
    {
        var shell = TestSession.Create();
        var (session, engine, note) = shell;
        engine.StoredNote = "text";
        engine.StoredNoteEditedAt = "2026-08-17T10:31:00Z";
        engine.StoredPatient = "sheet";
        engine.StoredPatientGeneratedAt = "2026-08-17T10:24:00Z";

        Assert.True(await session.OpenStoredSessionAsync("abc"));
        Assert.True(shell.Patient.PatientStale, "the sheet predates the note edit");

        engine.StoredPatientGeneratedAt = "2026-08-17T10:32:00Z";
        await session.CloseReviewAsync();
        Assert.False(shell.Patient.PatientStale, "leaving clears it");
        Assert.True(await session.OpenStoredSessionAsync("abc"));
        Assert.False(shell.Patient.PatientStale, "a sheet rewritten after the edit is current");

        engine.StoredNoteEditedAt = null;
        engine.StoredPatient = "Take two tablets a day.";
        engine.StoredPatientGeneratedAt = "2026-08-17T10:24:00Z";
        engine.StoredTranslation = "Jedna tabletka dziennie.";
        engine.StoredTranslatedAt = "2026-08-17T10:26:00Z";
        engine.StoredPatientEditedAt = "2026-08-17T10:31:00Z";
        await session.CloseReviewAsync();
        Assert.True(await session.OpenStoredSessionAsync("abc"));
        Assert.Equal("Jedna tabletka dziennie.", shell.Patient.TranslationText);
        Assert.True(shell.Patient.TranslationStale, "the sheet was edited after it was translated");
        Assert.True(shell.Commands.TranslateAgainCommand.CanExecute(null));

        engine.StoredPatientEditedAt = null;
        await session.CloseReviewAsync();
        Assert.True(await session.OpenStoredSessionAsync("abc"));
        Assert.False(shell.Patient.TranslationStale, "translated after the sheet was written");

        engine.StoredPatientGeneratedAt = "2026-08-17T10:40:00Z";
        await session.CloseReviewAsync();
        Assert.True(await session.OpenStoredSessionAsync("abc"));
        Assert.True(shell.Patient.TranslationStale, "the sheet was rewritten after it was translated");
    }

    [Fact]
    public async Task OpeningAnotherStartingAnewOrClosingMidEditAllSaveTheEdits()
    {
        var shell = TestSession.Create();
        var (session, engine, note) = shell;
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
        var note = new NoteViewModel(new AppPreferences(new MemoryPreferencesStore()));
        var optionChanges = 0;
        note.OptionsChanged += () => optionChanges++;

        note.LoadStored("text", "soap", "concise", "Edited 10:31");

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
        var preferences = new AppPreferences(new MemoryPreferencesStore()) { KeepConsultations = false };
        var shell = TestSession.Create(preferences);
        var (session, engine, _) = shell;

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

    private static System.Text.Json.JsonElement Translation(string text, string language) =>
        System.Text.Json.JsonSerializer.SerializeToElement(new { text, language });

    private sealed class CountingReviewActions : IReviewActions
    {
        public int Saves { get; private set; }

        public int Regenerates { get; private set; }

        public Task SaveNoteAsync()
        {
            Saves++;
            return Task.CompletedTask;
        }

        public Task RegenerateNoteAsync()
        {
            Regenerates++;
            return Task.CompletedTask;
        }

        public Task SavePatientAsync() => Task.CompletedTask;

        public Task WriteNoteAnywayAsync() => Task.CompletedTask;

        public Task RegeneratePatientAsync() => Task.CompletedTask;

        public Task TranslateAsync(string language) => Task.CompletedTask;

        public Task ReflectAsync() => Task.CompletedTask;
    }
}
