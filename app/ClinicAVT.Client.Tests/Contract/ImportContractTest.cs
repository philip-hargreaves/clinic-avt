using System.Text.Json;
using ClinicAVT.Client.Tests.Support;

namespace ClinicAVT.Client.Tests.Contract;

/// <summary>
/// A recording made elsewhere, read and imported by the real engine. Media Foundation does the
/// decoding and CI runners may lack it, so these run in the local gates.
/// </summary>
[Collection("engine")]
[Trait("Requires", "MediaFoundation")]
public class ImportContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(60);

    // Long enough that its decode is still running when the next request is read
    private const int LongSeconds = 20 * 60;

    [Fact]
    public async Task AnImportAnswersAtOnceThenEndsSealedAndListedAtItsOwnTime()
    {
        var wav = SilenceWav.Write();
        var recording = Recording("tone.m4a");
        try
        {
            await using var engine =
                EngineProcess.Start($"LOCAL\\clinicavt-import-{Guid.NewGuid():N}", wav);
            await using var client = await engine.ConnectAsync();
            var end = ImportEnd(client);

            var info = await client.RequestAsync("recording/inspect", new { path = recording }, Timeout);
            var recordedAt = info.GetProperty("recordedAt").GetString();
            Assert.Equal("2026-09-26T13:05:00Z", recordedAt);
            Assert.InRange(info.GetProperty("seconds").GetDouble(), 0.9, 1.1);

            await Assert.ThrowsAnyAsync<Exception>(() => client.RequestAsync("session/import",
                new { path = recording, startedAt = "2099-01-01T09:00:00Z", retain = true }, Timeout));

            var reply = await client.RequestAsync("session/import",
                new { path = recording, startedAt = recordedAt, retain = true }, Timeout);
            var id = reply.GetProperty("sessionId").GetString();
            var (method, parameters) = await end.WaitAsync(Timeout);
            Assert.Equal("session/imported", method);
            Assert.Equal(id, parameters.GetProperty("sessionId").GetString());

            var row = (await client.RequestAsync("session/list", null, Timeout))
                .GetProperty("sessions").EnumerateArray().Single(s => s.GetProperty("id").GetString() == id);
            Assert.Equal(recordedAt, row.GetProperty("startedAt").GetString());
        }
        finally
        {
            File.Delete(wav);
        }
    }

    [Fact]
    public async Task ACancelledImportEndsAsCancelledAndLeavesNothing()
    {
        var wav = SilenceWav.Write();
        var recording = SilenceWav.Write(LongSeconds);
        try
        {
            await using var engine =
                EngineProcess.Start($"LOCAL\\clinicavt-import-{Guid.NewGuid():N}", wav);
            await using var client = await engine.ConnectAsync();
            var end = ImportEnd(client);

            // Sent behind the import on the pipe, so it lands while the file still decodes. The
            // stand-in models finish anything later in moments
            var importing = client.RequestAsync("session/import",
                new { path = recording, startedAt = "2026-09-26T13:05:00Z", retain = true }, Timeout);
            var cancelling = client.RequestAsync("session/cancel", null, Timeout);
            var reply = await importing;
            await cancelling;

            var (method, parameters) = await end.WaitAsync(Timeout);
            Assert.Equal("session/importFailed", method);
            Assert.Equal(reply.GetProperty("sessionId").GetString(), parameters.GetProperty("sessionId").GetString());
            Assert.Equal("cancelled", parameters.GetProperty("error").GetString());
            Assert.Empty((await client.RequestAsync("session/list", null, Timeout))
                .GetProperty("sessions").EnumerateArray());
        }
        finally
        {
            File.Delete(wav);
            File.Delete(recording);
        }
    }

    [Fact]
    public async Task AShellThatLeavesMidImportLeavesTheEngineToFinishIt()
    {
        var wav = SilenceWav.Write();
        var recording = SilenceWav.Write(LongSeconds);
        try
        {
            await using var engine =
                EngineProcess.Start($"LOCAL\\clinicavt-import-{Guid.NewGuid():N}", wav);
            await using (var client = await engine.ConnectAsync())
            {
                await client.RequestAsync("session/import",
                    new { path = recording, startedAt = "2026-09-26T13:05:00Z", retain = true }, Timeout);
                await client.RequestAsync("engine/exit", null, Timeout);
            }

            Assert.Equal(0, await engine.WaitForExitAsync(Timeout));
            Assert.Contains("finalise stored", engine.StandardError);
        }
        finally
        {
            File.Delete(wav);
            File.Delete(recording);
        }
    }

    // The import's last word, which may come before the answer to session/import
    private static Task<(string Method, JsonElement Params)> ImportEnd(PipeTransport client)
    {
        var end = new TaskCompletionSource<(string, JsonElement)>(TaskCreationOptions.RunContinuationsAsynchronously);
        client.NotificationReceived += (method, parameters) =>
        {
            if (method is "session/imported" or "session/importFailed")
            {
                end.TrySetResult((method, parameters));
            }
        };
        return end.Task;
    }

    private static string Recording(string name)
    {
        var dir = AppContext.BaseDirectory;
        while (dir is not null && !Directory.Exists(Path.Combine(dir, "engine", "tests", "fixtures", "recordings")))
        {
            dir = Path.GetDirectoryName(dir);
        }

        Assert.NotNull(dir);
        return Path.Combine(dir, "engine", "tests", "fixtures", "recordings", name);
    }
}
