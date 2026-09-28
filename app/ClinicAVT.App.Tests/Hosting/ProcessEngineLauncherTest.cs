using System.Diagnostics;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Platform;

namespace ClinicAVT.App.Tests.Hosting;

[Trait("Requires", "Processes")]
public class ProcessEngineLauncherTest
{
    private static readonly TimeSpan ExitWait = TimeSpan.FromSeconds(5);

    private static TaskCompletionSource WatchExit(IEngineProcess process)
    {
        var exited = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        process.Exited += () => exited.TrySetResult();
        return exited;
    }

    [Fact]
    public async Task ExtraArgumentsJoinTheCommandLineAndADeathIsReported()
    {
        // Split across base and extra, so a mangled join exits at once
        using var launcher = new ProcessEngineLauncher(
            Path.Combine(Environment.SystemDirectory, "ping.exe"), "-n",
            extraArguments: () => ["60", "127.0.0.1"]);
        using var process = launcher.Launch();
        var exited = WatchExit(process);

        await Task.Delay(500);
        Assert.False(process.HasExited);
        Process.GetProcessById(process.Id).Kill();
        await exited.Task.WaitAsync(ExitWait);
        Assert.True(process.HasExited);
        Assert.Equal(-1, process.ExitCode);
    }

    [Fact]
    public void MissingExecutableThrows()
    {
        using var launcher = new ProcessEngineLauncher(@"C:\does\not\exist\engine.exe");

        Assert.ThrowsAny<Exception>(launcher.Launch);
    }
}
