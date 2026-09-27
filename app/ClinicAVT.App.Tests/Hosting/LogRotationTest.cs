using ClinicAVT.App.Core.Hosting;

namespace ClinicAVT.App.Tests.Hosting;

public sealed class LogRotationTest : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("clinicavt-logs").FullName;

    private string Log(string name) => Path.Combine(_dir, name);

    public void Dispose() => Directory.Delete(_dir, recursive: true);

    [Fact]
    public void EachRotationShiftsRunsDownAndTheOldestFallsOffAtTheKeepLimit()
    {
        LogRotation.Rotate(Log("engine.log"), keep: 3, atBytes: 0);
        Assert.Empty(Directory.GetFiles(_dir));  // nothing to rotate yet

        for (var run = 1; run <= 4; run++)
        {
            File.WriteAllText(Log("engine.log"), $"run {run}");
            LogRotation.Rotate(Log("engine.log"), keep: 3, atBytes: 0);
        }

        Assert.False(File.Exists(Log("engine.log")), "the current run starts fresh");
        Assert.Equal("run 4", File.ReadAllText(Log("engine-1.log")));
        Assert.Equal("run 3", File.ReadAllText(Log("engine-2.log")));
        Assert.False(File.Exists(Log("engine-3.log")), "keep bounds the set");
    }

    // Twelve quick launches once rotated away the only log of a stuck process
    [Fact]
    public void ALogIsAppendedUntilItIsLarge()
    {
        File.WriteAllText(Log("engine.log"), "run 1");
        LogRotation.Rotate(Log("engine.log"), keep: 3, atBytes: 100);
        Assert.Equal("run 1", File.ReadAllText(Log("engine.log")));

        File.WriteAllText(Log("engine.log"), new string('x', 100));
        LogRotation.Rotate(Log("engine.log"), keep: 3, atBytes: 100);
        Assert.False(File.Exists(Log("engine.log")));
        Assert.True(File.Exists(Log("engine-1.log")));
    }
}
