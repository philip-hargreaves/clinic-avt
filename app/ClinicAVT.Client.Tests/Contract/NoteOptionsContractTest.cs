using ClinicAVT.Client.Tests.Support;

namespace ClinicAVT.Client.Tests.Contract;

/// <summary>note/options against the real engine: the two lengths, and the old middle one.</summary>
[Collection("engine")]
[Trait("Requires", "Engine")]
public class NoteOptionsContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task TheTwoLengthsAndTheOldStandardAreAcceptedAndNothingElse()
    {
        await using var engine = EngineProcess.Start($"LOCAL\\clinicavt-options-{Guid.NewGuid():N}");
        await using var client = await engine.ConnectAsync();

        foreach (var detail in new[] { "concise", "detailed", "standard" })
        {
            await client.RequestAsync("note/options", new { style = "prose", detail }, Timeout);
        }

        var error = await Assert.ThrowsAsync<EngineErrorException>(
            () => client.RequestAsync("note/options", new { style = "prose", detail = "brief" }, Timeout));
        Assert.Equal(-32602, error.Code);
    }
}
