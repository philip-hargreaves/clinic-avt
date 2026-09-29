using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Hosting;

public class EngineSupervisorTest
{
    private sealed class FakeProcess(int? diedWith = null, int id = 1234) : IEngineProcess
    {
        public event Action? Exited;

        public int Id => id;

        public bool HasExited { get; private set; } = diedWith is not null;

        public int ExitCode { get; private set; } = diedWith ?? 0;

        public void Crash(int exitCode)
        {
            HasExited = true;
            ExitCode = exitCode;
            Exited?.Invoke();
        }

        public void Dispose()
        {
        }
    }

    private sealed class FakeLauncher : IEngineLauncher
    {
        public List<FakeProcess> Launched { get; } = [];

        public bool FailNext { get; set; }

        public int? NextDiesWith { get; set; }

        public FakeProcess? Serving { get; set; }

        public bool Released { get; private set; }

        public IEngineProcess Launch()
        {
            if (FailNext)
            {
                FailNext = false;
                throw new InvalidOperationException("no engine");
            }

            var process = new FakeProcess(NextDiesWith);
            NextDiesWith = null;
            Launched.Add(process);
            return process;
        }

        public IEngineProcess? Adopt() => Serving;

        public void Release() => Released = true;
    }

    private sealed class FakeCrashLog : ICrashLog
    {
        public List<CrashReport> Reports { get; } = [];

        public void Record(CrashReport report) => Reports.Add(report);
    }

    private sealed class Harness
    {
        public FakeLauncher Launcher { get; } = new();

        public FakeSession Session { get; } = new();

        public FakeTimeProvider Clock { get; } = new();

        public FakeCrashLog Log { get; } = new();

        public List<EngineStatus> Statuses { get; } = [];

        public string? InFlight { get; set; }

        public int ExitRequests { get; private set; }

        public EngineSupervisor Host { get; }

        public Harness()
        {
            Host = new EngineSupervisor(Launcher, Session, Clock, Log, () => InFlight, () =>
            {
                ExitRequests++;
                return Task.CompletedTask;
            });
            Host.StatusChanged += Statuses.Add;
        }

        public FakeProcess Current => Launcher.Launched[^1];

        public void CrashAndWait(int exitCode)
        {
            var count = Launcher.Launched.Count;
            Current.Crash(exitCode);
            Clock.Advance(RestartPolicy.Backoff(count));
        }
    }

    [Fact]
    public async Task ACrashRestartsSilentlyIsLoggedAndTheNextOneWaits()
    {
        var h = new Harness();

        h.Host.Start();
        Assert.Equal(EngineStatus.Running, h.Host.Status);
        Assert.Single(h.Launcher.Launched);
        Assert.Equal(new[] { EngineStatus.Running }, h.Statuses);
        Assert.Equal(1234, h.Host.EnginePid);

        // A first crash after 90 s relaunches at once, and the report records the in-flight state
        h.Clock.Now += TimeSpan.FromSeconds(90);
        h.InFlight = "session/stop";
        h.Session.SessionPhase = "Finalising:Note";
        h.Current.Crash(-7);

        Assert.Equal(EngineStatus.Running, h.Host.Status);
        Assert.Equal(2, h.Launcher.Launched.Count);
        Assert.Equal(
            new[] { EngineStatus.Running, EngineStatus.Restarting, EngineStatus.Running },
            h.Statuses);
        var report = Assert.Single(h.Log.Reports);
        Assert.Equal(-7, report.ExitCode);
        Assert.Equal(TimeSpan.FromSeconds(90), report.Uptime);
        Assert.Equal(1, report.CrashCount);
        Assert.Equal(RecoveryAction.Restart, report.Action);
        Assert.Equal(h.Clock.Now, report.Timestamp);
        Assert.Equal("session/stop", report.MethodInFlight);
        Assert.Equal("Finalising:Note", report.SessionPhase);

        // A second crash in the window waits before relaunching. The engine only leaves on its
        // own once no shell is connected, so a clean exit under a running app is a crash too
        h.Current.Crash(0);
        Assert.Equal(EngineStatus.Restarting, h.Host.Status);
        Assert.Equal(2, h.Launcher.Launched.Count);
        Assert.Equal(0, h.Log.Reports[^1].ExitCode);
        h.Clock.Advance(RestartPolicy.Backoff(2));
        Assert.Equal(EngineStatus.Running, h.Host.Status);
        Assert.Equal(3, h.Launcher.Launched.Count);

        // Releasing during the wait cancels the relaunch
        h.Current.Crash(-1);
        Assert.Equal(EngineStatus.Restarting, h.Host.Status);
        await h.Host.ReleaseAsync();
        h.Clock.Advance(RestartPolicy.MaxBackoff);
        Assert.Equal(EngineStatus.Stopped, h.Host.Status);
        Assert.Equal(3, h.Launcher.Launched.Count);
    }

    [Fact]
    public void ACrashStormGivesUpAndStartRecoversButSpacedCrashesNeverTripIt()
    {
        var h = new Harness();
        h.Host.Start();

        for (var i = 0; i < RestartPolicy.StormLimit; i++)
        {
            h.CrashAndWait(-1);
        }

        Assert.Equal(EngineStatus.Faulted, h.Host.Status);
        Assert.Equal(RestartPolicy.StormLimit, h.Launcher.Launched.Count);
        Assert.Equal(RestartPolicy.StormLimit, h.Log.Reports.Count);
        Assert.Equal(RecoveryAction.GiveUp, h.Log.Reports[^1].Action);

        h.Host.Start();

        Assert.Equal(EngineStatus.Running, h.Host.Status);
        h.Current.Crash(-1);
        Assert.Equal(EngineStatus.Running, h.Host.Status);

        var spaced = new Harness();
        spaced.Host.Start();
        for (var i = 0; i < RestartPolicy.StormLimit + 3; i++)
        {
            spaced.Current.Crash(-1);
            spaced.Clock.Now += RestartPolicy.StormWindow + TimeSpan.FromSeconds(1);
        }

        Assert.Equal(EngineStatus.Running, spaced.Host.Status);
    }

    [Fact]
    public async Task ReleaseLetsTheEngineOutliveTheApp()
    {
        var h = new Harness();
        h.Host.Start();

        await h.Host.ReleaseAsync();

        Assert.Equal(1, h.ExitRequests);
        Assert.True(h.Launcher.Released);
        Assert.Equal(EngineStatus.Stopped, h.Host.Status);
    }

    // A closed app's engine may still be finishing a load, and the next app adopts it
    [Fact]
    public void AnEngineStillServingIsTakenOverRatherThanCounted()
    {
        var h = new Harness();
        h.Launcher.NextDiesWith = EngineSupervisor.AlreadyServing;
        h.Launcher.Serving = new FakeProcess(id: 4321);

        h.Host.Start();

        Assert.Equal(EngineStatus.Running, h.Host.Status);
        Assert.Equal(4321, h.Host.EnginePid);
        Assert.Empty(h.Log.Reports);
        Assert.Single(h.Launcher.Launched);
    }

    // A reopened app finds its old engine busy with a first compile. It holds the pipe but
    // cannot be adopted until it finishes. The app waits for it instead of counting crashes
    [Fact]
    public void AnEngineThatCannotBeAdoptedYetIsWaitedForNotCounted()
    {
        var h = new Harness();
        h.Launcher.NextDiesWith = EngineSupervisor.AlreadyServing;

        h.Host.Start();
        for (var i = 0; i < RestartPolicy.StormLimit * 3; i++)
        {
            Assert.Equal(EngineStatus.Restarting, h.Host.Status);
            h.Launcher.NextDiesWith = EngineSupervisor.AlreadyServing;
            h.Clock.Advance(EngineSupervisor.ServerWait);
        }

        Assert.Empty(h.Log.Reports);

        h.Launcher.Serving = new FakeProcess(id: 4321);
        h.Launcher.NextDiesWith = EngineSupervisor.AlreadyServing;
        h.Clock.Advance(EngineSupervisor.ServerWait);

        Assert.Equal(EngineStatus.Running, h.Host.Status);
        Assert.Equal(4321, h.Host.EnginePid);
        Assert.Empty(h.Log.Reports);
    }

    [Fact]
    public void WaitingForAnotherEngineEndsAsACrashAfterTheLimit()
    {
        var h = new Harness();
        h.Launcher.NextDiesWith = EngineSupervisor.AlreadyServing;
        h.Host.Start();

        while (h.Log.Reports.Count == 0)
        {
            h.Launcher.NextDiesWith = EngineSupervisor.AlreadyServing;
            h.Clock.Advance(EngineSupervisor.ServerWait);
        }

        Assert.Equal(EngineSupervisor.AlreadyServing, h.Log.Reports[0].ExitCode);
        Assert.True(h.Clock.Now >= DateTimeOffset.UnixEpoch + EngineSupervisor.ServerWaitLimit);
    }

    // A launch that throws is a fault, and an engine that dies before the handler attaches is
    // still seen and relaunched
    [Fact]
    public void StartFaultsOnALaunchFailureAndStillCatchesAnEarlyDeath()
    {
        var failed = new Harness();
        failed.Launcher.FailNext = true;
        failed.Host.Start();
        Assert.Equal(EngineStatus.Faulted, failed.Host.Status);
        Assert.Empty(failed.Log.Reports);

        var early = new Harness();
        early.Launcher.NextDiesWith = 1;
        early.Host.Start();
        Assert.Equal(EngineStatus.Running, early.Host.Status);
        Assert.Equal(2, early.Launcher.Launched.Count);
    }
}
