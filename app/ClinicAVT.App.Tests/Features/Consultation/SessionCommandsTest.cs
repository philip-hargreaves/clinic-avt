using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using static ClinicAVT.App.Tests.Support.Waits;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Features.Consultation;

public class SessionCommandsTest
{
    [Fact]
    public async Task CommandsDriveTheMachineAndCanExecuteVisibilityAndTheClockFollowTheState()
    {
        var shell = TestSession.Create();
        var (session, engine, _) = shell;
        var controls = shell.Get<SessionControlsViewModel>();

        // Recording waits for the engine
        engine.SetConnected(false);
        Assert.False(controls.StartRecordingCommand.CanExecute(null));
        engine.SetConnected(true);
        Assert.True(controls.StartRecordingCommand.CanExecute(null));

        Assert.False(controls.StopRecordingCommand.CanExecute(null));
        Assert.False(controls.FinishConsultationCommand.CanExecute(null));
        Assert.True(controls.IdleVisible);
        Assert.True(controls.CentreStageVisible);
        Assert.False(controls.PanesVisible);
        Assert.False(controls.ReviewVisible);
        Assert.True(controls.MicPickerVisible);

        await controls.StartRecordingCommand.ExecuteAsync(null);
        Assert.Equal(SessionState.Recording, session.State);
        Assert.False(controls.StartRecordingCommand.CanExecute(null));
        Assert.True(controls.StopRecordingCommand.CanExecute(null));
        Assert.True(controls.CancelRecordingCommand.CanExecute(null));
        Assert.False(controls.IdleVisible);
        Assert.True(controls.RecordingVisible);
        Assert.True(controls.CentreStageVisible);
        Assert.False(controls.MicPickerVisible, "gone from Record until Finish consultation");

        // The clock and the ring follow delivered audio
        for (var i = 0; i < 754; i++)
        {
            engine.RaiseNotification("audio.level", Params(new { level = 0.5, clipped = false }));
        }

        Assert.Equal("01:15", controls.ElapsedLabel);
        Assert.Equal(0.5, controls.Level);

        // Once sealed and before the note streams, the centre holds and says why
        await controls.StopRecordingCommand.ExecuteAsync(null);
        Assert.True(controls.CentreStageVisible);
        Assert.True(controls.FinalisingVisible);
        Assert.False(controls.PanesVisible);
        Assert.Equal("Preparing note", controls.FinalisingLabel);
        Assert.False(controls.MicPickerVisible);

        // The first token opens the panes, with the note already filling
        engine.RaiseNotification("note/partial", Params(new { text = "The" }));
        Assert.True(controls.PanesVisible);
        Assert.False(controls.FinalisingVisible);
        Assert.False(controls.MicPickerVisible, "not while the note streams");
        engine.RaiseNotification("note/ready");
        Assert.True(controls.ReviewVisible);
        Assert.False(controls.RecordingVisible);
        Assert.False(controls.CentreStageVisible);
        Assert.True(controls.FinishConsultationCommand.CanExecute(null));
        Assert.False(controls.MicPickerVisible, "the cell is Finish consultation's now");

        controls.FinishConsultationCommand.Execute(null);
        Assert.Equal(SessionState.Idle, session.State);
        Assert.True(controls.IdleVisible);
        Assert.True(controls.MicPickerVisible, "back for the next consultation");
    }

    // No partial streams for a thin recording, so note/ready must open the panes on its own or
    // the centre would spin forever, and nothing may claim to be writing
    [Fact]
    public async Task AThinRecordingOpensThePanesOnTheCannedNoteAndNeverClaimsToBeWriting()
    {
        var log = new ListLogger();
        var shell = TestSession.Create(log: log);
        var (session, engine, _) = shell;
        var controls = shell.Get<SessionControlsViewModel>();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        Assert.True(controls.CentreStageVisible);

        engine.RaiseNotification("note/ready", Params(new { text = "The recording was too short." }));
        engine.RaiseNotification("patient/ready", Params(new { text = "The recording was too short." }));

        Assert.True(controls.PanesVisible);
        Assert.True(controls.ReviewVisible);
        Assert.DoesNotContain(log.Lines, l => l.Contains("Writing", StringComparison.Ordinal));
        Assert.Equal("Ready for review", shell.Line.LatestActivity);
    }

    [Fact]
    public async Task FirstTimeSetupBlocksRecordingUntilModelsCompile()
    {
        var shell = TestSession.Create(
            engine: new FakeEngineClient(autoNotify: false) { FirstUse = true, ModelsCompiled = false });
        var (session, engine, _) = shell;
        var bar = shell.Status;
        var controls = shell.Get<SessionControlsViewModel>();
        shell.Host.RaiseStatus(ClinicAVT.App.Core.Hosting.EngineStatus.Running);

        Assert.False(session.ModelsReady);
        Assert.False(controls.StartRecordingCommand.CanExecute(null));
        Assert.True(bar.ShowsSetup);
        Assert.False(bar.Busy, "the bar stands in for the ring");
        Assert.Equal("First-time setup · 0:00 · optimising for your PC", bar.DisplayLabel);
        shell.Activity.Listening = true;
        Assert.False(bar.ShowsSetup, "the level meter has the bar's place");
        shell.Activity.StopListening();

        engine.ModelsCompiled = true;
        shell.Clock.Advance(TimeSpan.FromSeconds(2));
        await WaitUntilAsync(() => session.ModelsReady);

        Assert.True(session.ModelsReady);
        Assert.True(controls.StartRecordingCommand.CanExecute(null));
        Assert.False(bar.ShowsSetup);
        Assert.Equal("Ready", bar.DisplayLabel);
    }

    [Fact]
    public void AWarmLaunchAndAWarmNoteModelLoadNeverGateRecording()
    {
        var shell = TestSession.Create();
        var (session, engine, _) = shell;
        var controls = shell.Get<SessionControlsViewModel>();
        Assert.True(session.ModelsReady);
        Assert.Equal(1, engine.Requests.Count(r => r.Method == "engine/readiness"));
        shell.Host.RaiseStatus(ClinicAVT.App.Core.Hosting.EngineStatus.Running);

        engine.RaiseNotification("note/model", System.Text.Json.JsonSerializer.SerializeToElement(
            new { tier = "default", id = "qwen3.5-9b-int4", name = "Qwen3.5 9B", state = "loading", firstUse = false }));

        Assert.True(session.ModelsReady);
        Assert.True(controls.StartRecordingCommand.CanExecute(null));
        Assert.Equal("Ready", shell.Status.DisplayLabel);
        Assert.Equal("Ready to start", controls.StartLabel);

        // A first-use compile holds recording, and only then does the status line show it
        engine.RaiseNotification("note/model", System.Text.Json.JsonSerializer.SerializeToElement(
            new { tier = "default", id = "qwen3.5-9b-int4", name = "Qwen3.5 9B", state = "loading", firstUse = true }));
        Assert.False(session.ModelsReady);
        Assert.StartsWith("First-time setup", shell.Status.DisplayLabel);

        engine.RaiseNotification("note/model", System.Text.Json.JsonSerializer.SerializeToElement(
            new { tier = "default", id = "qwen3.5-9b-int4", name = "Qwen3.5 9B", state = "ready" }));
        Assert.True(session.ModelsReady);
        Assert.Equal("Ready", shell.Status.DisplayLabel);
    }

    [Fact]
    public async Task FinaliseStagesNameTheCentreSpinner()
    {
        var shell = TestSession.Create();
        var (session, engine, _) = shell;
        var controls = shell.Get<SessionControlsViewModel>();
        var phases = new List<FinalisePhase>();
        session.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(ConsultationViewModel.Phase))
            {
                phases.Add(session.Phase);
            }
        };

        await session.StartRecordingAsync();
        engine.RaiseNotification("session/progress", Params(new { stage = "speakers" }));
        Assert.Equal(FinalisePhase.None, session.Phase);  // ignored before finalising

        // The engine reports its stages while session/stop blocks
        engine.BeforeReply = method =>
        {
            if (method != "session/stop")
            {
                return;
            }

            Assert.Equal("Finalising", controls.FinalisingLabel);
            engine.RaiseNotification("session/progress", Params(new { stage = "transcript" }));
            Assert.Equal("Writing transcript", controls.FinalisingLabel);
            engine.RaiseNotification("session/progress", Params(new { stage = "unknown" }));
            Assert.Equal("Writing transcript", controls.FinalisingLabel);  // an unknown stage changes nothing
            engine.RaiseNotification("session/progress", Params(new { stage = "speakers" }));
            Assert.Equal("Labelling speakers", controls.FinalisingLabel);
            // The per-turn re-decode is transcript work again, and on the
            // NPU the longest stage, so the spinner says what it is doing
            engine.RaiseNotification("session/progress", Params(new { stage = "turns" }));
            Assert.Equal("Writing transcript", controls.FinalisingLabel);
        };
        await session.StopRecordingAsync();
        engine.BeforeReply = null;

        // Once sealed the caption names the prefill, and a late stage cannot go back
        Assert.Equal(FinalisePhase.Note, session.Phase);
        Assert.Equal("Preparing note", controls.FinalisingLabel);
        // A note that waits on the model's load says so
        engine.RaiseNotification("note/model", Params(new { tier = "default", state = "loading" }));
        Assert.Equal("Waiting for the note model · 0:00", controls.FinalisingLabel);
        engine.RaiseNotification("note/model", Params(new { tier = "default", state = "ready" }));
        Assert.Equal("Preparing note", controls.FinalisingLabel);
        engine.RaiseNotification("session/progress", Params(new { stage = "transcript" }));
        Assert.Equal(FinalisePhase.Note, session.Phase);
        // Start resets to None, then the stop walks forward only
        Assert.Collection(phases.SkipWhile(p => p == FinalisePhase.None),
            p => Assert.Equal(FinalisePhase.Sealing, p),
            p => Assert.Equal(FinalisePhase.Transcript, p),
            p => Assert.Equal(FinalisePhase.Speakers, p),
            p => Assert.Equal(FinalisePhase.Turns, p),
            p => Assert.Equal(FinalisePhase.Note, p));

        // The next stop starts from the beginning again
        engine.RaiseNotification("note/partial", Params(new { text = "The" }));
        Assert.Equal(FinalisePhase.Streaming, session.Phase);
        engine.RaiseNotification("note/ready", Params(new { text = "note" }));
        engine.RaiseNotification("patient/ready", Params(new { text = "sheet" }));
        session.FinishConsultation();
        Assert.Equal(FinalisePhase.None, session.Phase);
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        Assert.Equal(FinalisePhase.Note, session.Phase);
    }

    // A resume the engine dies during stays recording, and the next reconnect retries it
    [Fact]
    public async Task ARestartedEngineResumesTheLiveSessionUntilItTakesButNothingWhileIdle()
    {
        var shell = TestSession.Create();
        var (session, engine, _) = shell;

        engine.SetConnected(false);
        engine.SetConnected(true);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/start");
        Assert.Equal(SessionState.Idle, session.State);

        await session.StartRecordingAsync();
        engine.FailNext = method => method == "session/start"
            ? new IOException("pipe transport is closed") : null;
        engine.SetConnected(false);
        engine.SetConnected(true);
        Assert.Equal(SessionState.Recording, session.State);
        Assert.Equal("Recovering", shell.Line.LatestActivity);

        engine.SetConnected(false);
        engine.SetConnected(true);

        var starts = engine.Requests.Where(r => r.Method == "session/start").ToList();
        Assert.Equal(3, starts.Count);
        Assert.Contains("resume", starts[2].Params);
        Assert.Contains("s1", starts[2].Params);
        Assert.Equal(SessionState.Recording, session.State);
        Assert.Equal("Recording", shell.Line.LatestActivity);
    }

    [Fact]
    public async Task AResumeTheEngineRefusesKeepsTheSessionAndStopsRecording()
    {
        var shell = TestSession.Create();
        var (session, engine, _) = shell;
        await session.StartRecordingAsync();

        engine.FailNext = method => method == "session/start"
            ? new ClinicAVT.Client.EngineErrorException(-32000, "no stored audio", null) : null;
        engine.SetConnected(false);
        engine.SetConnected(true);

        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal("Could not resume - consultation kept", shell.Line.LatestActivity);
    }
}
