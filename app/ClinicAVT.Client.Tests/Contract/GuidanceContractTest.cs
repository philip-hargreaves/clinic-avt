using System.Text.Json;
using ClinicAVT.Client.Tests.Support;

namespace ClinicAVT.Client.Tests.Contract;

/// <summary>
/// The guidance contract as the CI engine can show it. No embedding model is staged, so the
/// feature reports itself unavailable, a search fails with the loader's reason and a recorded
/// session carries no record.
/// </summary>
[Collection("engine")]
[Trait("Requires", "Engine")]
public class GuidanceContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task WithoutAnEmbedderTheFeatureSaysSoAndSearchesFailCleanly()
    {
        var wav = SilenceWav.Write();
        try
        {
            await using var engine =
                EngineProcess.Start($"LOCAL\\clinicavt-guidance-{Guid.NewGuid():N}", wav);
            await using var client = await engine.ConnectAsync();
            var model = new TaskCompletionSource<JsonElement>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            var failed = new TaskCompletionSource<JsonElement>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            var patientReady = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            client.NotificationReceived += (method, parameters) =>
            {
                switch (method)
                {
                    case "guidance/model":
                        model.TrySetResult(parameters);
                        break;
                    case "guidance/failed":
                        failed.TrySetResult(parameters);
                        break;
                    case "patient/ready":
                        patientReady.TrySetResult();
                        break;
                }
            };

            // The embedder loads on its own thread, so an early client gets loading and then
            // guidance/model
            var corpora = await client.RequestAsync("guidance/corpora", null, Timeout);
            if (corpora.GetProperty("state").GetString() == "loading")
            {
                var ended = await model.Task.WaitAsync(Timeout);
                Assert.Equal("unavailable", ended.GetProperty("state").GetString());
                Assert.False(string.IsNullOrEmpty(ended.GetProperty("detail").GetString()));
                corpora = await client.RequestAsync("guidance/corpora", null, Timeout);
            }
            Assert.Equal("unavailable", corpora.GetProperty("state").GetString());
            Assert.False(string.IsNullOrEmpty(corpora.GetProperty("detail").GetString()));
            Assert.Equal(0, corpora.GetProperty("corpora").GetArrayLength());

            var reply = await client.RequestAsync(
                "guidance/search", new { text = "Chest pain on exertion." }, Timeout);
            Assert.Equal(JsonValueKind.Object, reply.ValueKind);
            var failure = await failed.Task.WaitAsync(Timeout);
            Assert.Equal(JsonValueKind.Null, failure.GetProperty("id").ValueKind);
            Assert.False(string.IsNullOrEmpty(failure.GetProperty("detail").GetString()));

            // No note model, so no note and nothing searched. The record is absent
            await client.RequestAsync("session/start", null, Timeout);
            await client.RequestAsync("session/stop", null, Timeout);
            await patientReady.Task.WaitAsync(Timeout);
            var list = await client.RequestAsync("session/list", null, Timeout);
            var id = list.GetProperty("sessions")[0].GetProperty("id").GetString()!;
            var guidance = await client.RequestAsync("session/guidance", new { id }, Timeout);
            Assert.Equal(JsonValueKind.Null, guidance.GetProperty("guidance").ValueKind);
        }
        finally
        {
            File.Delete(wav);
        }
    }
}
