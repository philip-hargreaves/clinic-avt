using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.Support;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Preferences;

public class NoteOptionsTest
{
    // The engine never reads a preference. It loads the tier the shell names on
    // connect, before the options and before readiness is asked
    [Fact]
    public void StartupPushesTheTierFirstThenThePersistedOptionsAndAChangePersistsAndReachesTheEngine()
    {
        var path = Path.Combine(Path.GetTempPath(), $"clinicavt-test-{Guid.NewGuid():N}", "preferences.json");
        var preferences = new AppPreferences(path)
        {
            NoteStyle = "soap",
            NoteDetail = "detailed",
            NoteTier = "accuracy",
        };

        var (_, engine, note) = TestSession.Create(preferences);

        Assert.Equal("soap", note.Style);
        Assert.Equal("detailed", note.Detail);
        var push = engine.Requests.Single(r => r.Method == "note/options");
        Assert.Contains("soap", push.Params);
        Assert.Contains("detailed", push.Params);

        var methods = engine.Requests.Select(r => r.Method).ToList();
        var tier = engine.Requests.Single(r => r.Method == "note/tier");
        Assert.Contains("accuracy", tier.Params);
        Assert.True(methods.IndexOf("note/tier") < methods.IndexOf("note/options"));
        Assert.True(methods.IndexOf("note/tier") < methods.IndexOf("engine/readiness"));

        note.Detail = "concise";

        var saved = AppPreferences.Load(path);
        Assert.Equal(("soap", "concise"), (saved.NoteStyle, saved.NoteDetail));
        Assert.Contains(engine.Requests, r => r.Method == "note/options" && r.Params.Contains("concise"));
        Directory.Delete(Path.GetDirectoryName(path)!, recursive: true);
    }

    [Fact]
    public async Task SaveWaitsForTheSealedDocumentsThenRegenerateRewritesWithTheCurrentOptions()
    {
        var (session, engine, note) = TestSession.Create();
        Assert.False(note.SaveNoteCommand.CanExecute(null));
        Assert.False(note.RegenerateCommand.CanExecute(null));

        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/partial", Params(new { text = "streaming" }));
        Assert.False(note.SaveNoteCommand.CanExecute(null));
        Assert.False(note.SavePatientCommand.CanExecute(null));

        engine.RaiseNotification("note/ready", Params(new { text = "the note" }));
        Assert.False(note.SaveNoteCommand.CanExecute(null));
        engine.RaiseNotification("patient/ready", Params(new { text = "the sheet" }));
        Assert.True(note.SaveNoteCommand.CanExecute(null));
        Assert.True(note.SavePatientCommand.CanExecute(null));

        note.ClinicalNoteText = "edited note";
        note.PatientInfoText = "edited sheet";
        await note.SaveNoteCommand.ExecuteAsync(null);
        await note.SavePatientCommand.ExecuteAsync(null);

        var noteSave = engine.Requests.Single(r => r.Method == "note/update");
        Assert.Contains("s1", noteSave.Params);
        Assert.Contains("edited note", noteSave.Params);
        var patientSave = engine.Requests.Single(r => r.Method == "patient/update");
        Assert.Contains("edited sheet", patientSave.Params);
        Assert.Equal("Patient information saved", session.Status.LatestActivity);

        note.Style = "soap";
        Assert.True(note.RegenerateCommand.CanExecute(null));
        await note.RegenerateCommand.ExecuteAsync(null);
        Assert.True(note.RegenerateWarningOpen, "the note was edited, so regenerate asks first");
        await note.ConfirmRegenerateCommand.ExecuteAsync(null);

        var request = engine.Requests.Single(r => r.Method == "note/regenerate");
        Assert.Contains("soap", request.Params);
        Assert.Equal(NotePipelineState.NoteWriting, note.PipelineState);
        Assert.Equal("", note.ClinicalNoteText);
        Assert.False(note.RegenerateCommand.CanExecute(null));

        // The rewrite streams while the shell already sits in review
        engine.RaiseNotification("note/partial", Params(new { text = "S:" }));
        Assert.Equal("S:", note.ClinicalNoteText);
        engine.RaiseNotification("note/ready", Params(new { text = "S: headache" }));
        engine.RaiseNotification("patient/ready", Params(new { text = "sheet 2" }));

        Assert.Equal(SessionState.Review, session.State);
        Assert.Equal(NotePipelineState.AllReady, note.PipelineState);
        Assert.Equal("S: headache", note.ClinicalNoteText);
        Assert.True(note.RegenerateCommand.CanExecute(null));
    }
}
