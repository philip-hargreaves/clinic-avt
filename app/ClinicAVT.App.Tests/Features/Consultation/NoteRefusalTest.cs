using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Tests.Support;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Features.Consultation;

public class NoteRefusalTest
{
    [Fact]
    public async Task ARefusalStaysOnTheRecordScreenSaysWhyAndOffersToWriteAnyway()
    {
        var shell = TestSession.Create();
        var (session, engine, note) = shell;
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("session/progress", Params(new { stage = "transcript" }));
        engine.RaiseNotification("note/refused", Params(new { reason = "a cooking video" }));

        Assert.Equal(NotePipelineState.NoteRefused, note.PipelineState);
        Assert.True(note.NoteRefused);
        Assert.Equal("a cooking video", note.RefusalReason);
        Assert.Equal(SessionState.Refused, session.State);
        Assert.StartsWith("No note", shell.Line.LatestActivity);
        Assert.True(note.CanWriteAnyway);
        Assert.True(shell.Commands.WriteAnywayCommand.CanExecute(null));

        await shell.Commands.WriteAnywayCommand.ExecuteAsync(null);

        var request = Assert.Single(engine.Requests, r => r.Method == "note/regenerate");
        Assert.Contains("\"confirmed\":true", request.Params);
        Assert.Equal(SessionState.Review, session.State);
        Assert.Equal(NotePipelineState.NoteWriting, note.PipelineState);
        Assert.Equal("", note.RefusalReason);
        Assert.False(note.NoteRefused);
    }

    [Fact]
    public async Task ATooShortRecordingIsRefusedWithoutTheOverrideAndDoneReturnsToIdle()
    {
        var shell = TestSession.Create();
        var (session, engine, note) = shell;
        var controls = shell.Get<SessionControlsViewModel>();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("session/progress", Params(new { stage = "transcript" }));
        engine.RaiseNotification("note/refused",
            Params(new { reason = "12 words; a note needs at least 25", overridable = false }));

        Assert.True(note.NoteRefused);
        Assert.Equal("12 words; a note needs at least 25", note.RefusalReason);
        Assert.False(note.CanWriteAnyway);
        Assert.False(shell.Commands.WriteAnywayCommand.CanExecute(null));
        Assert.Equal(SessionState.Refused, session.State);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "note/regenerate");
        Assert.True(controls.RefusedVisible);
        Assert.True(controls.CentreStageVisible);
        Assert.True(controls.DoneCommand.CanExecute(null));

        await controls.DoneCommand.ExecuteAsync(null);

        Assert.Contains(engine.Requests, r => r.Method == "session/close");
        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal("", note.RefusalReason);
        Assert.False(controls.RefusedVisible);
    }
}
