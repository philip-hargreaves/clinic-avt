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
        using var shell = TestSession.Offline(log);
        var bar = shell.Status;
        var line = shell.Line;
        var state = shell.Get<EngineState>();

        shell.Host.RaiseStatus(EngineStatus.Running);
        Assert.Equal("Starting up", state.EngineStateLabel);
        Assert.True(state.EngineStarting);

        shell.Engine.SetConnected(true);
        Assert.Equal("Ready", state.EngineStateLabel);
        Assert.False(state.EngineStarting);

        shell.Engine.SetConnected(false);
        shell.Host.RaiseStatus(EngineStatus.Restarting);
        Assert.Equal("Recovering", state.EngineStateLabel);

        shell.Host.RaiseStatus(EngineStatus.Stopped);
        Assert.Equal("Not running", state.EngineStateLabel);
        Assert.Empty(log.Lines);  // only faults reach the log

        // Once back up, activity replaces the status and the ring means work in progress
        shell.Host.RaiseStatus(EngineStatus.Running);
        shell.Engine.SetConnected(true);
        Assert.Equal("Ready", bar.DisplayLabel);
        Assert.False(bar.Busy);

        line.Append("Finalising", busy: true);
        Assert.Equal("Finalising", bar.DisplayLabel);
        Assert.True(bar.Busy);

        line.Append("Ready for review");
        Assert.Equal("Ready for review", bar.DisplayLabel);
        Assert.False(bar.Busy);

        // Abnormal readiness outranks whatever activity was showing
        shell.Engine.SetConnected(false);
        shell.Host.RaiseStatus(EngineStatus.Restarting);
        Assert.Equal("Recovering", bar.DisplayLabel);
        Assert.True(bar.Busy);

        // Every fault is logged
        var logged = log.Lines.Count;
        shell.Host.RaiseStatus(EngineStatus.Faulted);
        Assert.Equal("Recording is unavailable - please restart the app", state.EngineStateLabel);
        Assert.Equal("Recording is unavailable - please restart the app", line.LatestActivity);

        shell.Host.RaiseStatus(EngineStatus.Faulted);
        Assert.Equal("Recording is unavailable - please restart the app", state.EngineStateLabel);
        Assert.Equal(logged + 2, log.Lines.Count);
    }

    // A storage fault line stays through other activity until the next consultation
    [Fact]
    public async Task AStorageFaultHoldsTheLineUntilTheNextConsultation()
    {
        var log = new ListLogger();
        var shell = TestSession.Create(log: log);
        var (session, engine, _) = shell;
        var bar = shell.Status;
        shell.Host.RaiseStatus(EngineStatus.Running);

        engine.RaiseNotification("storage/fault", Fixtures.Load("storage-fault.json").GetProperty("params"));
        const string Line =
            "Can't save to disk: audio commit: database or disk is full. Recording continues; free some space.";
        Assert.Equal(Line, bar.DisplayLabel);
        Assert.Contains(log.Lines, line => line.EndsWith(Line, StringComparison.Ordinal));

        shell.Line.Append("Ready for review");
        Assert.Equal(Line, bar.DisplayLabel);

        await session.StartRecordingAsync();
        Assert.Equal("Recording", bar.DisplayLabel);
    }

    // A cancel puts up its own status line, and a recording can still start. A consultation is
    // in progress from Record until its note is written, then only on screen for review
    [Fact]
    public async Task TheConsentReminderShowsWhileARecordingCouldStartAndInProgressLastsUntilTheNote()
    {
        var shell = TestSession.Create();
        var (session, engine, _) = shell;
        var bar = shell.Status;
        engine.SetConnected(false);
        shell.Host.RaiseStatus(EngineStatus.Running);
        Assert.False(bar.ConsentVisible);
        engine.SetConnected(true);
        Assert.True(bar.ConsentVisible);
        Assert.False(session.ConsultationInProgress);

        shell.Models.SetSettingUp(true);
        Assert.False(bar.ConsentVisible);
        shell.Models.SetSettingUp(false);

        await session.StartRecordingAsync();
        Assert.False(bar.ConsentVisible);
        Assert.True(session.ConsultationInProgress);
        await session.CancelRecordingAsync();
        Assert.Equal("Cancelled", bar.DisplayLabel);
        Assert.True(bar.ConsentVisible);

        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        Assert.False(bar.ConsentVisible);
        Assert.True(session.ConsultationInProgress);
        engine.RaiseNotification("note/ready");
        Assert.False(bar.ConsentVisible);
        Assert.False(session.ConsultationInProgress);
        Assert.NotNull(session.ReviewedSessionId);

        await session.EndReviewAsync();
        Assert.True(bar.ConsentVisible);
        Assert.Null(session.ReviewedSessionId);
        Assert.False(session.ConsultationInProgress);
    }

    [Fact]
    public void AStoreFromAnotherVersionIsExplainedAndLogged()
    {
        var log = new ListLogger();
        using var shell = TestSession.Offline(log);
        var state = shell.Get<EngineState>();

        shell.Host.RaiseStatus(EngineStatus.StoreNewer);
        Assert.Equal(
            "Your consultations were saved by a newer version of ClinicAVT. Update ClinicAVT to open them.",
            shell.Line.LatestActivity);
        Assert.False(state.EngineStarting);

        shell.Host.RaiseStatus(EngineStatus.StoreTooOld);
        Assert.Equal(
            "Your consultations were saved by a version of ClinicAVT too old for this one to open.",
            state.EngineStateLabel);
        Assert.Equal(2, log.Lines.Count);
    }
}
