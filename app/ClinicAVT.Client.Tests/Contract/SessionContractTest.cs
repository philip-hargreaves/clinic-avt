using ClinicAVT.Client.Tests.Support;

namespace ClinicAVT.Client.Tests.Contract;

/// <summary>
/// The session methods and the notifications they produce, against the real
/// engine on a private pipe.
/// </summary>
[Collection("engine")]
[Trait("Requires", "Engine")]
public class SessionContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    // The stopped session reports the three finalise stages (transcript, speakers,
    // turns), then the documents. The cancelled one reports nothing
    private static readonly string[] ExpectedNotifications =
        ["session/progress", "session/progress", "session/progress", "note/ready", "patient/ready"];

    [Fact]
    public async Task StopProducesTheNoteThenPatientNotifications()
    {
        var wav = SilenceWav.Write();
        try
        {
            await using var engine =
                EngineProcess.Start($"LOCAL\\clinicavt-session-{Guid.NewGuid():N}", wav);
            var notifications = new List<string>();
            var patientReady = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);

            await using (var client = await engine.ConnectAsync())
            {
                client.NotificationReceived += (method, parameters) =>
                {
                    lock (notifications)
                    {
                        notifications.Add(method);
                    }

                    if (method == "patient/ready")
                    {
                        patientReady.TrySetResult();
                    }
                };

                // A stale micId must never stop a session starting. The
                // engine resolves it against what exists and falls back
                await client.RequestAsync("session/start", System.Text.Json.JsonSerializer
                    .SerializeToElement(new { micId = "{unplugged-device}" }), Timeout);
                await client.RequestAsync("session/cancel", null, Timeout);
                await client.RequestAsync("session/start", null, Timeout);
                await client.RequestAsync("session/stop", null, Timeout);

                await patientReady.Task.WaitAsync(Timeout);
                lock (notifications)
                {
                    // Sessions stream levels and the embedder announces itself once.
                    // The pipeline ordering holds among the rest
                    Assert.Contains("audio.level", notifications);
                    Assert.Equal(
                        ExpectedNotifications,
                        notifications
                            .Where(n => n is not ("audio.level" or "guidance/model"))
                            .ToArray());
                }

                await client.RequestAsync("engine/exit", null, Timeout);
            }

            // Asked to leave, the engine goes once its shell disconnects. Unasked, it would
            // wait for the shell to come back
            Assert.Equal(0, await engine.WaitForExitAsync(Timeout));

            // Of the two sessions, the cancelled one left nothing and the stopped
            // one is in the database. The readback test checks its contents
            Assert.True(File.Exists(Path.Combine(engine.StoreRoot, "clinicavt.db")));
            Assert.False(Directory.Exists(Path.Combine(engine.StoreRoot, "sessions")));
        }
        finally
        {
            File.Delete(wav);
        }
    }
}
