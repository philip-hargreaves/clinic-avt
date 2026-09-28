using System.Text.Json;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Features.Documents;

public class NotePipelineTest
{
    private static JsonElement Text(string text) =>
        JsonSerializer.SerializeToElement(new { text });

    private static JsonElement Detail(string detail) =>
        JsonSerializer.SerializeToElement(new { detail });

    [Fact]
    public void ThePipelineRefusesOutOfOrderEventsAndPreparingShowsUntilTheFirstWordsStream()
    {
        var note = new NoteViewModel();
        Assert.False(note.NotePreparing);

        Assert.False(note.Apply(NotePipelineEvent.NoteReady));
        Assert.False(note.Apply(NotePipelineEvent.PatientInfoReady));
        Assert.Equal(NotePipelineState.Pending, note.PipelineState);

        Assert.True(note.Apply(NotePipelineEvent.NoteWritingStarted));
        Assert.True(note.NotePreparing);
        Assert.True(note.PatientPreparing);
        Assert.Equal("Writing the note", note.NoteStateCaption);
        Assert.False(note.Apply(NotePipelineEvent.PatientInfoReady));
        Assert.Equal(NotePipelineState.NoteWriting, note.PipelineState);

        note.ClinicalNoteText = "The patient";
        Assert.False(note.NotePreparing);
        Assert.True(note.PatientPreparing);

        Assert.True(note.Apply(NotePipelineEvent.NoteReady));
        Assert.Equal(NotePipelineState.NoteReadyPatientWriting, note.PipelineState);
        note.PatientInfoText = "Your appointment";
        Assert.False(note.PatientPreparing);

        Assert.True(note.Apply(NotePipelineEvent.PatientInfoReady));
        Assert.Equal(NotePipelineState.AllReady, note.PipelineState);
    }

    [Fact]
    public async Task BothDocumentsStreamAndSealWithReviewAtNoteReadyAndWritingStatusesOnlyWhileTokensStream()
    {
        var log = new ListLogger();
        var (session, engine, note) = TestSession.Create(log: log);
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();

        engine.RaiseNotification("note/partial", Text("The patient"));
        Assert.Equal("The patient", note.ClinicalNoteText);
        Assert.Equal("Writing clinical note", session.Status.LatestActivity);

        engine.RaiseNotification("note/partial", Text("The patient presents"));
        Assert.Equal(1, log.Lines.Count(l => l.EndsWith("Writing clinical note", StringComparison.Ordinal)));
        engine.RaiseNotification("note/ready", Text("The patient presents with bursitis."));
        Assert.Equal("The patient presents with bursitis.", note.ClinicalNoteText);
        Assert.Equal(SessionState.Review, session.State);
        Assert.Equal(NotePipelineState.NoteReadyPatientWriting, note.PipelineState);

        engine.RaiseNotification("patient/partial", Text("Your appointment"));
        Assert.Equal("Your appointment", note.PatientInfoText);
        Assert.Equal("Writing patient information", session.Status.LatestActivity);

        engine.RaiseNotification("patient/ready", Text("Your appointment today ..."));
        Assert.Equal("Your appointment today ...", note.PatientInfoText);
        Assert.Equal(NotePipelineState.AllReady, note.PipelineState);

        session.FinishConsultation();
        Assert.Equal(NotePipelineState.Pending, note.PipelineState);
    }

    [Fact]
    public async Task AFailedNoteStillReachesReviewWithACaptionPointingAtTheStatusBar()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();

        engine.RaiseNotification("note/failed", Detail("the transcript is empty"));

        Assert.Equal(SessionState.Review, session.State);
        Assert.Equal(NotePipelineState.NoteFailed, note.PipelineState);
        Assert.False(note.NotePreparing);
        Assert.Contains("could not be written", note.NoteStateCaption);
        Assert.True(note.NoteCaptionVisible);
    }

    [Fact]
    public async Task AFailedPatientSheetHasItsOwnState()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready", Text("the note"));

        engine.RaiseNotification("patient/failed", Detail("generation failed"));

        Assert.Equal(NotePipelineState.PatientFailed, note.PipelineState);
        Assert.Equal(SessionState.Review, session.State);
    }

    [Fact]
    public async Task TranslationWaitsForTheStoredSheetFlowsThroughTheEngineAndAFailureReleasesTheButton()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready", Text("the note"));
        Assert.Contains("Polish", note.Languages);
        note.SelectedLanguage = "Polish";

        // While the sheet streams the pane has text but the engine has stored
        // nothing, so a request now would error
        engine.RaiseNotification("patient/partial", Text("Your appointment"));
        Assert.False(note.TranslateCommand.CanExecute(null));

        engine.RaiseNotification("patient/ready", Text("Your appointment today ..."));
        Assert.True(note.TranslateCommand.CanExecute(null));
        await note.TranslateCommand.ExecuteAsync(null);

        var request = engine.Requests.Single(r => r.Method == "patient/translate");
        Assert.Contains("Polish", request.Params);
        Assert.Contains("s1", request.Params);

        Assert.False(note.TranslateCommand.CanExecute(null), "a second translation waits for the first");
        engine.RaiseNotification("translate/partial", Text("Twoja"));
        engine.RaiseNotification("translate/ready",
            JsonSerializer.SerializeToElement(new { text = "Twoja wizyta", language = "Polish" }));
        Assert.Equal("Twoja wizyta", note.TranslationText);
        Assert.True(note.TranslateCommand.CanExecute(null));

        note.SelectedLanguage = "French";
        await note.TranslateCommand.ExecuteAsync(null);
        Assert.False(note.TranslateCommand.CanExecute(null));
        engine.RaiseNotification("translate/failed", Detail("boom"));

        Assert.True(note.TranslateCommand.CanExecute(null));
    }

    [Fact]
    public async Task AThinRecordingNeverClaimsToBeWriting()
    {
        var log = new ListLogger();
        var (session, engine, _) = TestSession.Create(log: log);
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();

        // The engine's graceful statement arrives with no partials at all
        engine.RaiseNotification("note/ready", Text("The recording was too short."));
        engine.RaiseNotification("patient/ready", Text("The recording was too short."));

        Assert.DoesNotContain(log.Lines, l => l.Contains("Writing", StringComparison.Ordinal));
        Assert.Equal("Ready for review", session.Status.LatestActivity);
    }
}
