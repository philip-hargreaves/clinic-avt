using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Shell;

public class EngineStatusInShellTest
{
    [Fact]
    public void TheStatusBarThroughAnEngineLifetime()
    {
        var log = new ListLogger();
        var bar = TestSession.Status(log: log);

        bar.SetEngineState(EngineStatus.Running);
        Assert.Equal("Starting up", bar.EngineStateLabel);
        Assert.True(bar.EngineStarting);

        bar.SetEngineReady(true);
        Assert.Equal("Ready", bar.EngineStateLabel);
        Assert.False(bar.EngineStarting);

        bar.SetEngineReady(false);
        bar.SetEngineState(EngineStatus.Restarting);
        Assert.Equal("Recovering", bar.EngineStateLabel);

        bar.SetEngineState(EngineStatus.Stopped);
        Assert.Equal("Not running", bar.EngineStateLabel);
        Assert.Empty(log.Lines);  // only faults reach the log

        // Once back up, activity replaces the status and the ring means work in progress
        bar.SetEngineState(EngineStatus.Running);
        bar.SetEngineReady(true);
        Assert.Equal("Ready", bar.DisplayLabel);
        Assert.False(bar.Busy);

        bar.Append("Finalising", busy: true);
        Assert.Equal("Finalising", bar.DisplayLabel);
        Assert.True(bar.Busy);

        bar.Append("Ready for review");
        Assert.Equal("Ready for review", bar.DisplayLabel);
        Assert.False(bar.Busy);

        // Abnormal readiness outranks whatever activity was showing
        bar.SetEngineReady(false);
        bar.SetEngineState(EngineStatus.Restarting);
        Assert.Equal("Recovering", bar.DisplayLabel);
        Assert.True(bar.Busy);

        // Every fault is logged
        var logged = log.Lines.Count;
        bar.SetEngineState(EngineStatus.Faulted);
        Assert.Equal("Recording is unavailable - please restart the app", bar.EngineStateLabel);
        Assert.Equal("Recording is unavailable - please restart the app", bar.LatestActivity);

        bar.SetEngineState(EngineStatus.Faulted);
        Assert.Equal("Recording is unavailable - please restart the app", bar.EngineStateLabel);
        Assert.Equal(logged + 2, log.Lines.Count);
    }

    // A full disk must not pass unnoticed. The line stays through other activity until the
    // next consultation starts
    [Fact]
    public async Task AStorageFaultHoldsTheLineUntilTheNextConsultation()
    {
        var log = new ListLogger();
        var (session, engine, _) = TestSession.Create(log: log);
        var bar = session.Status;
        bar.SetEngineState(EngineStatus.Running);
        bar.SetEngineReady(true);

        engine.RaiseNotification("storage/fault", Fixtures.Load("storage-fault.json").GetProperty("params"));
        const string Line =
            "Can't save to disk: audio commit: database or disk is full. Recording continues; free some space.";
        Assert.Equal(Line, bar.DisplayLabel);
        Assert.Contains(log.Lines, line => line.EndsWith(Line, StringComparison.Ordinal));

        bar.Append("Ready for review");
        Assert.Equal(Line, bar.DisplayLabel);

        await session.StartRecordingAsync();
        Assert.Equal("Recording", bar.DisplayLabel);
    }

    [Fact]
    public async Task InProgressLastsFromRecordUntilTheNoteIsWritten()
    {
        var (session, engine, _) = TestSession.Create();
        Assert.False(session.ConsultationInProgress);

        await session.StartRecordingAsync();
        Assert.True(session.ConsultationInProgress);

        await session.StopRecordingAsync();
        Assert.True(session.ConsultationInProgress);

        // Written, it is only on screen for review
        engine.RaiseNotification("note/ready");
        Assert.False(session.ConsultationInProgress);
        Assert.NotNull(session.ReviewedSessionId);

        await session.EndReviewAsync();
        Assert.Null(session.ReviewedSessionId);
        Assert.False(session.ConsultationInProgress);
    }
}
