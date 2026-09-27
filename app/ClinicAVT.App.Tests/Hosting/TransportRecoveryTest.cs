using System.Diagnostics;
using ClinicAVT.App.Tests.Support;
using static ClinicAVT.App.Tests.Support.Waits;

namespace ClinicAVT.App.Tests.Hosting;

/// <summary>
/// The supervisor, launcher and connection together against the real engine.
/// A kill while idle must heal without a new client.
/// </summary>
[Collection("engine")]
[Trait("Requires", "Engine")]
public class TransportRecoveryTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task TheConnectionSurvivesASupervisedRestart()
    {
        // Echo tests the transport alone and needs no microphone on a
        // CI runner
        await using var engine = await RealEngine.StartAsync("e2e", models: false);
        var (host, connection) = (engine.Host, engine.Connection);
        var firstPid = host.EnginePid;
        Assert.NotNull(firstPid);

        Process.GetProcessById(firstPid.Value).Kill();

        await RetryAsync(() => connection.RequestAsync("engine/echo", new { payload = "back" }, Timeout));
        Assert.NotEqual(firstPid, host.EnginePid);
        Assert.Single(File.ReadAllLines(engine.CrashLog));

        var patientReady = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        connection.NotificationReceived += (method, _) =>
        {
            if (method == "patient/ready")
            {
                patientReady.TrySetResult();
            }
        };
        await connection.RequestAsync("session/stop", null, Timeout);
        await patientReady.Task.WaitAsync(Timeout);
    }
}
