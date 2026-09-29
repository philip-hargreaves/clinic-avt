using System.Diagnostics;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Platform;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Hosting;

/// <summary>
/// Runs the supervision loop on real processes. A stand-in engine is killed from outside and must
/// recover.
/// </summary>
[Trait("Requires", "Processes")]
public class SupervisionIntegrationTest
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

    private sealed class RecordingLauncher(IEngineLauncher inner) : IEngineLauncher
    {
        public List<IEngineProcess> Launched { get; } = [];

        public IEngineProcess Launch()
        {
            var process = inner.Launch();
            Launched.Add(process);
            return process;
        }

        public IEngineProcess? Adopt() => inner.Adopt();

        public void Release() => inner.Release();
    }

    private sealed class Rig : IDisposable
    {
        private readonly ProcessEngineLauncher _inner = new(
            Path.Combine(Environment.SystemDirectory, "ping.exe"), "-n 60 127.0.0.1");

        private readonly string _directory =
            Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());

        public RecordingLauncher Launcher { get; }

        public FakeSession Session { get; } = new();

        public string CrashPath { get; }

        public EngineSupervisor Host { get; }

        public Rig()
        {
            CrashPath = Path.Combine(_directory, "crashes.jsonl");
            Launcher = new RecordingLauncher(_inner);
            Host = new EngineSupervisor(
                Launcher, Session, TimeProvider.System, new FileCrashLog(CrashPath));
        }

        public int Pid(int launchIndex) => Launcher.Launched[launchIndex].Id;

        public void Dispose()
        {
            Host.Dispose();
            DisposeLauncher();
            if (Directory.Exists(_directory))
            {
                Directory.Delete(_directory, recursive: true);
            }
        }

        // Disposing the job object kills any process not released from it
        public void DisposeLauncher() => _inner.Dispose();
    }

    private static Task WhenStatusAsync(IEngineHost host, EngineStatus wanted)
    {
        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        host.StatusChanged += OnChanged;
        return tcs.Task;

        void OnChanged(EngineStatus status)
        {
            if (status == wanted)
            {
                host.StatusChanged -= OnChanged;
                tcs.TrySetResult();
            }
        }
    }

    private static async Task WaitUntilGoneAsync(int pid)
    {
        for (var i = 0; i < 100; i++)
        {
            try
            {
                using var process = Process.GetProcessById(pid);
            }
            catch (ArgumentException)
            {
                return;
            }

            await Task.Delay(50);
        }

        Assert.Fail($"process {pid} is still running");
    }

    [Fact]
    public async Task KillingTheEngineTriggersARealRestart()
    {
        using var rig = new Rig();
        rig.Host.Start();
        var firstPid = rig.Pid(0);

        var restarted = WhenStatusAsync(rig.Host, EngineStatus.Running);
        Process.GetProcessById(firstPid).Kill();
        await restarted.WaitAsync(Wait);

        Assert.Equal(2, rig.Launcher.Launched.Count);
        var secondPid = rig.Pid(1);
        Assert.NotEqual(firstPid, secondPid);
        using (var replacement = Process.GetProcessById(secondPid))
        {
            Assert.False(replacement.HasExited);
        }

        Assert.Single(File.ReadAllLines(rig.CrashPath));

        rig.Host.Dispose();
        rig.DisposeLauncher();
        await WaitUntilGoneAsync(secondPid);
    }

    [Fact]
    public async Task AnAppThatEndsLeavesNothingBehind()
    {
        using var rig = new Rig();
        rig.Host.Start();
        var pid = rig.Pid(0);

        rig.Host.Dispose();
        rig.DisposeLauncher();

        await WaitUntilGoneAsync(pid);
        Assert.False(File.Exists(rig.CrashPath));
    }
}
