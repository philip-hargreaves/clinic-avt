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
    public async Task StopBringsTheLabelledTranscriptThenBothDocumentsStreamAndSealWithWritingStatusesOnlyWhileTokensStream()
    {
        var log = new ListLogger();
        var (session, engine, note) = TestSession.Create(log: log);
        engine.Transcript.Add(("doctor", "how long has the knee been swollen"));
        engine.Transcript.Add(("patient", "about three weeks now"));
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();

        Assert.Equal(["doctor", "patient"], session.Transcript.Turns.Select(t => t.Speaker));
        Assert.Equal("Doctor", session.Transcript.Turns[0].SpeakerLabel);
        Assert.Equal("how long has the knee been swollen", session.Transcript.Turns[0].Text);

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
    public async Task AFailedNoteStillReachesReviewWithACaptionAndAFailedSheetHasItsOwnState()
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

        session.FinishConsultation();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready", Text("the note"));
        engine.RaiseNotification("patient/failed", Detail("generation failed"));

        Assert.Equal(NotePipelineState.PatientFailed, note.PipelineState);
        Assert.Equal(SessionState.Review, session.State);
    }

    [Fact]
    public async Task TranslationWaitsForTheStoredSheetFlowsThroughTheEngineAndAFailureOrALostEngineReleasesTheButton()
    {
        var log = new ListLogger();
        var (session, engine, note) = TestSession.Create(log: log);
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

        await note.TranslateCommand.ExecuteAsync(null);
        Assert.True(note.TranslationRunning);
        engine.SetConnected(false);
        Assert.False(note.TranslationRunning);
        Assert.False(session.EngineReady);
        Assert.Contains(log.Lines, line => line.Contains("connection lost"));
    }
}
