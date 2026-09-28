using System.Text.Json;
using ClinicAVT.Client.Tests.Support;

namespace ClinicAVT.Client.Tests.Contract;

/// <summary>
/// The model list and the note lane's settings against the real engine with no weights staged.
/// The parameter checks and the lane-absent answer come from the engine itself.
/// </summary>
[Collection("engine")]
[Trait("Requires", "Engine")]
public class NoteTierContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task WithNothingStagedTheListIsEmptyAndTheNoteSettingsAreCheckedByTheEngine()
    {
        await using var engine = EngineProcess.Start($"LOCAL\\clinicavt-tier-{Guid.NewGuid():N}");
        await using var client = await engine.ConnectAsync();

        var models = (await client.RequestAsync("engine/models", null, Timeout)).GetProperty("models");
        Assert.Equal(JsonValueKind.Array, models.ValueKind);
        Assert.Equal(0, models.GetArrayLength());

        // The two lengths and the old middle one are accepted, and nothing else
        foreach (var detail in new[] { "concise", "detailed", "standard" })
        {
            await client.RequestAsync("note/options", new { style = "prose", detail }, Timeout);
        }

        var error = await Assert.ThrowsAsync<EngineErrorException>(
            () => client.RequestAsync("note/options", new { style = "prose", detail = "brief" }, Timeout));
        Assert.Equal(-32602, error.Code);

        error = await Assert.ThrowsAsync<EngineErrorException>(
            () => client.RequestAsync("note/tier", new { tier = "premium" }, Timeout));
        Assert.Equal(-32602, error.Code);

        // A real tier, or automatic, finds no lane to load
        foreach (var tier in new[] { "default", "auto" })
        {
            error = await Assert.ThrowsAsync<EngineErrorException>(
                () => client.RequestAsync("note/tier", new { tier }, Timeout));
            Assert.NotEqual(-32601, error.Code);
            Assert.Contains("no note model", error.ErrorData?.GetString() ?? "",
                StringComparison.OrdinalIgnoreCase);
        }
    }
}
