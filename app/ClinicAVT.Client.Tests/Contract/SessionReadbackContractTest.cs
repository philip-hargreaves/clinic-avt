using System.Text.Json;
using ClinicAVT.Client.Tests.Support;

namespace ClinicAVT.Client.Tests.Contract;

[Collection("engine")]
[Trait("Requires", "Engine")]
public class SessionReadbackContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task RecordedSessionsAreListedReadableAndDeletable()
    {
        var wav = SilenceWav.Write();
        try
        {
            await using var engine =
                EngineProcess.Start($"LOCAL\\clinicavt-readback-{Guid.NewGuid():N}", wav);
            await using var client = await engine.ConnectAsync();
            // The note and sheet follow the seal on their own thread, so wait for them
            var patientReady = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            client.NotificationReceived += (method, _) =>
            {
                if (method == "patient/ready")
                {
                    patientReady.TrySetResult();
                }
            };

            await client.RequestAsync("session/start", null, Timeout);
            await client.RequestAsync("session/stop", null, Timeout);
            await patientReady.Task.WaitAsync(Timeout);

            var list = await client.RequestAsync("session/list", null, Timeout);
            var sessions = list.GetProperty("sessions");
            Assert.Equal(1, sessions.GetArrayLength());
            Assert.False(string.IsNullOrEmpty(sessions[0].GetProperty("endedAt").GetString()));
            var id = sessions[0].GetProperty("id").GetString()!;

            var transcript = await client.RequestAsync(
                "session/transcript", new { id }, Timeout);
            var turns = transcript.GetProperty("turns");
            Assert.Equal(1, turns.GetArrayLength());
            // The sealed transcript is tidied, so the scripted text starts with a capital
            Assert.StartsWith("Scripted turn 0", turns[0].GetProperty("text").GetString());
            Assert.Equal(0, turns[0].GetProperty("firstFrame").GetInt64());
            // Stop may land before the fast replay finishes, so the tail holds
            // whatever had arrived. Two seconds of audio is the ceiling
            Assert.InRange(turns[0].GetProperty("frameCount").GetInt64(), 1, 32000);

            // The CI engine has no note model, so the record starts empty with
            // every field present and fills through the clinician's edits
            var note = await client.RequestAsync("session/note", new { id }, Timeout);
            Assert.Equal("", note.GetProperty("text").GetString());
            Assert.Equal(JsonValueKind.Null, note.GetProperty("generatedAt").ValueKind);
            Assert.Equal(JsonValueKind.Null, note.GetProperty("editedAt").ValueKind);
            Assert.Equal("", sessions[0].GetProperty("label").GetString());

            // A WinUI text box ends its lines with CR alone. The store keeps LF
            await client.RequestAsync("note/update", new { id, text = "Plan\redited\r\n" }, Timeout);
            note = await client.RequestAsync("session/note", new { id }, Timeout);
            Assert.Equal("Plan\nedited\n", note.GetProperty("text").GetString());
            Assert.False(string.IsNullOrEmpty(note.GetProperty("editedAt").GetString()));

            var patient = await client.RequestAsync("session/patient", new { id }, Timeout);
            Assert.Equal(JsonValueKind.Null, patient.GetProperty("translation").ValueKind);

            await client.RequestAsync("session/label", new { id, text = "Elbow swelling" }, Timeout);
            var relisted = await client.RequestAsync("session/list", null, Timeout);
            var row = relisted.GetProperty("sessions")[0];
            Assert.Equal("Elbow swelling", row.GetProperty("label").GetString());
            Assert.False(string.IsNullOrEmpty(row.GetProperty("editedAt").GetString()));

            // A past session reopens for regeneration. The CI engine has no
            // note model, so regenerate is refused cleanly, and close ends the
            // review
            await client.RequestAsync("session/open", new { id }, Timeout);
            await Assert.ThrowsAnyAsync<Exception>(
                () => client.RequestAsync(
                    "note/regenerate", new { style = "prose", detail = "concise" }, Timeout));
            await client.RequestAsync("session/close", null, Timeout);
            await Assert.ThrowsAnyAsync<Exception>(
                () => client.RequestAsync("session/open", new { id = "nope" }, Timeout));

            await client.RequestAsync("session/delete", new { id }, Timeout);

            var after = await client.RequestAsync("session/list", null, Timeout);
            Assert.Equal(0, after.GetProperty("sessions").GetArrayLength());

            // With keep consultations off a session is readable by id until it
            // is left, absent from history and erased at close
            await client.RequestAsync("session/start", new { retain = false }, Timeout);
            var unretainedId = (await client.RequestAsync("session/stop", null, Timeout))
                .GetProperty("sessionId").GetString()!;
            var unretained = await client.RequestAsync("session/list", null, Timeout);
            Assert.Equal(0, unretained.GetProperty("sessions").GetArrayLength());
            var transcript2 = await client.RequestAsync(
                "session/transcript", new { id = unretainedId }, Timeout);
            Assert.True(transcript2.GetProperty("turns").GetArrayLength() > 0);
            await client.RequestAsync("session/close", null, Timeout);
            await Assert.ThrowsAnyAsync<Exception>(
                () => client.RequestAsync("session/transcript", new { id = unretainedId }, Timeout));
            Assert.True(File.Exists(Path.Combine(engine.StoreRoot, "clinicavt.db")));
            Assert.False(Directory.Exists(Path.Combine(engine.StoreRoot, "sessions")));

            await Assert.ThrowsAnyAsync<Exception>(
                () => client.RequestAsync("session/delete", new { id }, Timeout));
        }
        finally
        {
            File.Delete(wav);
        }
    }
}
