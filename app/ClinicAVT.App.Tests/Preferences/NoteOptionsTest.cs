using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Preferences;

public class NoteOptionsTest
{
    // The engine never reads a preference. It loads the tier the shell names on
    // connect, before the options and before readiness is asked
    [Fact]
    public void StartupPushesTheTierFirstThenThePersistedOptionsAndAChangePersistsAndReachesTheEngine()
    {
        var store = new MemoryPreferencesStore();
        var preferences = new AppPreferences(store)
        {
            NoteStyle = "soap",
            NoteDetail = "detailed",
            NoteTier = "accuracy",
        };

        var shell = TestSession.Create(preferences);
        var (_, engine, note) = shell;

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

        var saved = AppPreferences.Load(store);
        Assert.Equal(("soap", "concise"), (saved.NoteStyle, saved.NoteDetail));
        Assert.Contains(engine.Requests, r => r.Method == "note/options" && r.Params.Contains("concise"));
    }

    [Fact]
    public async Task SaveWaitsForTheSealedDocumentsThenRegenerateRewritesWithTheCurrentOptions()
    {
        var shell = TestSession.Create();
        var (session, engine, note) = shell;
        Assert.False(shell.Commands.SaveNoteCommand.CanExecute(null));
        Assert.False(shell.Commands.RegenerateCommand.CanExecute(null));

        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/partial", Params(new { text = "streaming" }));
        Assert.False(shell.Commands.SaveNoteCommand.CanExecute(null));
        Assert.False(shell.Commands.SavePatientCommand.CanExecute(null));

        engine.RaiseNotification("note/ready", Params(new { text = "the note" }));
        Assert.False(shell.Commands.SaveNoteCommand.CanExecute(null));
        engine.RaiseNotification("patient/ready", Params(new { text = "the sheet" }));
        Assert.True(shell.Commands.SaveNoteCommand.CanExecute(null));
        Assert.True(shell.Commands.SavePatientCommand.CanExecute(null));

        note.ClinicalNoteText = "edited note";
        shell.Patient.PatientInfoText = "edited sheet";
        await shell.Commands.SaveNoteCommand.ExecuteAsync(null);
        await shell.Commands.SavePatientCommand.ExecuteAsync(null);

        var noteSave = engine.Requests.Single(r => r.Method == "note/update");
        Assert.Contains("s1", noteSave.Params);
        Assert.Contains("edited note", noteSave.Params);
        var patientSave = engine.Requests.Single(r => r.Method == "patient/update");
        Assert.Contains("edited sheet", patientSave.Params);
        Assert.Equal("Patient information saved", shell.Line.LatestActivity);

        note.Style = "soap";
        Assert.True(shell.Commands.RegenerateCommand.CanExecute(null));
        await shell.Commands.RegenerateCommand.ExecuteAsync(null);
        Assert.True(note.RegenerateWarningOpen, "the note was edited, so regenerate asks first");
        await shell.Commands.ConfirmRegenerateCommand.ExecuteAsync(null);

        var request = engine.Requests.Single(r => r.Method == "note/regenerate");
        Assert.Contains("soap", request.Params);
        Assert.Equal(NotePipelineState.NoteWriting, note.PipelineState);
        Assert.Equal("", note.ClinicalNoteText);
        Assert.False(shell.Commands.RegenerateCommand.CanExecute(null));

        // The rewrite streams while the shell already sits in review
        engine.RaiseNotification("note/partial", Params(new { text = "S:" }));
        Assert.Equal("S:", note.ClinicalNoteText);
        engine.RaiseNotification("note/ready", Params(new { text = "S: headache" }));
        engine.RaiseNotification("patient/ready", Params(new { text = "sheet 2" }));

        Assert.Equal(SessionState.Review, session.State);
        Assert.Equal(NotePipelineState.AllReady, note.PipelineState);
        Assert.Equal("S: headache", note.ClinicalNoteText);
        Assert.True(shell.Commands.RegenerateCommand.CanExecute(null));
    }
}
