using System.Text.Json;
using ClinicAVT.Client.Tests.Support;

namespace ClinicAVT.Client.Tests.Contract;

/// <summary>
/// Back up, delete everything and restore, against the real engine and file.
/// </summary>
[Collection("engine")]
[Trait("Requires", "Engine")]
public class BackupContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);
    // A real backup derives its key with 600,000 rounds, twice with the check
    private static readonly TimeSpan JobTimeout = TimeSpan.FromSeconds(60);
    private const string Password = "maple-orbit-fender-quill-harbor";

    [Fact]
    public async Task ABackupRestoresWhatDeleteAllErasedAndOnlyOnce()
    {
        var wav = SilenceWav.Write();
        var folder = Directory.CreateTempSubdirectory("clinicavt-backup-");
        var path = Path.Combine(folder.FullName, "backup.clinicavt");
        try
        {
            await using var engine =
                EngineProcess.Start($"LOCAL\\clinicavt-backup-{Guid.NewGuid():N}", wav);
            await using var client = await engine.ConnectAsync();
            var patientReady = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource<(string Method, JsonElement Params)> job = new(
                TaskCreationOptions.RunContinuationsAsynchronously);
            client.NotificationReceived += (method, parameters) =>
            {
                if (method == "patient/ready")
                {
                    patientReady.TrySetResult();
                }
                else if (method is "archive/done" or "archive/failed")
                {
                    job.TrySetResult((method, parameters.Clone()));
                }
            };

            async Task<(string Method, JsonElement Params)> RunAsync(string method, object request)
            {
                job = new(TaskCreationOptions.RunContinuationsAsynchronously);
                await client.RequestAsync(method, request, Timeout);
                return await job.Task.WaitAsync(JobTimeout);
            }

            await client.RequestAsync("session/start", null, Timeout);
            await client.RequestAsync("session/stop", null, Timeout);
            await patientReady.Task.WaitAsync(Timeout);
            var id = (await client.RequestAsync("session/list", null, Timeout))
                .GetProperty("sessions")[0].GetProperty("id").GetString()!;
            await client.RequestAsync("note/update", new { id, text = "The clinician's note" }, Timeout);
            await client.RequestAsync("session/label", new { id, text = "Elbow swelling" }, Timeout);
            var transcript = (await client.RequestAsync("session/transcript", new { id }, Timeout))
                .GetProperty("turns").GetRawText();

            var summary = await client.RequestAsync("archive/summary", new { from = "", to = "" }, Timeout);
            Assert.Equal(1, summary.GetProperty("consultations").GetInt32());

            var backup = await RunAsync(
                "archive/backup", new { from = "", to = "", path, password = Password });
            Assert.Equal("archive/done", backup.Method);
            Assert.Equal(1, backup.Params.GetProperty("consultations").GetInt32());
            Assert.Equal(id, backup.Params.GetProperty("ids")[0].GetString());
            Assert.True(File.Exists(path));
            Assert.Empty(Directory.GetFiles(folder.FullName, "*.partial"));
            Assert.True(File.ReadAllBytes(path).AsSpan().IndexOf("The clinician's note"u8) < 0,
                "the backup holds no plaintext");

            await client.RequestAsync("session/deleteAll", new { deleteReflections = true }, Timeout);
            Assert.Equal(0, (await client.RequestAsync("session/list", null, Timeout))
                .GetProperty("sessions").GetArrayLength());

            var wrong = await RunAsync(
                "archive/restore", new { path, password = "not-the-password-at-all", dryRun = false });
            Assert.Equal("archive/failed", wrong.Method);
            Assert.Equal("wrong-password", wrong.Params.GetProperty("code").GetString());

            var preview = await RunAsync("archive/restore", new { path, password = Password, dryRun = true });
            Assert.Equal(1, preview.Params.GetProperty("consultations").GetInt32());
            Assert.Equal(0, (await client.RequestAsync("session/list", null, Timeout))
                .GetProperty("sessions").GetArrayLength());

            var restore = await RunAsync("archive/restore", new { path, password = Password, dryRun = false });
            Assert.Equal("archive/done", restore.Method);
            Assert.Equal(1, restore.Params.GetProperty("consultations").GetInt32());
            var row = (await client.RequestAsync("session/list", null, Timeout))
                .GetProperty("sessions")[0];
            Assert.Equal(id, row.GetProperty("id").GetString());
            Assert.Equal("Elbow swelling", row.GetProperty("label").GetString());
            Assert.Equal("The clinician's note",
                (await client.RequestAsync("session/note", new { id }, Timeout))
                    .GetProperty("text").GetString());
            Assert.Equal(transcript, (await client.RequestAsync("session/transcript", new { id }, Timeout))
                .GetProperty("turns").GetRawText());

            var again = await RunAsync("archive/restore", new { path, password = Password, dryRun = false });
            Assert.Equal(0, again.Params.GetProperty("consultations").GetInt32());
            Assert.Equal(1, again.Params.GetProperty("skipped").GetInt32());
        }
        finally
        {
            File.Delete(wav);
            folder.Delete(recursive: true);
        }
    }
}
