namespace ClinicAVT.App.Platform.Tests;

public sealed class LogRotationTest : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("clinicavt-logs").FullName;

    private string Log(string name) => Path.Combine(_dir, name);

    public void Dispose() => Directory.Delete(_dir, recursive: true);

    // Rotate by size, not per launch, so quick relaunches cannot rotate away a stuck process's log
    [Fact]
    public void ALogIsAppendedUntilLargeAndEachRotationShiftsRunsDownToTheKeepLimit()
    {
        LogRotation.Rotate(Log("engine.log"), keep: 3, atBytes: 0);
        Assert.Empty(Directory.GetFiles(_dir));

        File.WriteAllText(Log("engine.log"), "run 1");
        LogRotation.Rotate(Log("engine.log"), keep: 3, atBytes: 100);
        Assert.Equal("run 1", File.ReadAllText(Log("engine.log")));

        File.WriteAllText(Log("engine.log"), new string('x', 100));
        LogRotation.Rotate(Log("engine.log"), keep: 3, atBytes: 100);
        Assert.False(File.Exists(Log("engine.log")));
        Assert.True(File.Exists(Log("engine-1.log")));

        for (var run = 2; run <= 4; run++)
        {
            File.WriteAllText(Log("engine.log"), $"run {run}");
            LogRotation.Rotate(Log("engine.log"), keep: 3, atBytes: 0);
        }

        Assert.False(File.Exists(Log("engine.log")), "the current run starts fresh");
        Assert.Equal("run 4", File.ReadAllText(Log("engine-1.log")));
        Assert.Equal("run 3", File.ReadAllText(Log("engine-2.log")));
        Assert.False(File.Exists(Log("engine-3.log")), "keep bounds the set");
    }
}
