using System.IO.Pipes;
using System.Text.Json;
using ClinicAVT.Client.Tests.Support;

namespace ClinicAVT.Client.Tests.Contract;

/// <summary>
/// The real shell client against the real engine process. This is the evidence
/// the two halves agree on the wire as well as against fixtures.
/// </summary>
[Collection("engine")]
[Trait("Requires", "Engine")]
public class ContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task HelloNamesTheEngineEchoKeepsNonAsciiIntactAndAnUnknownMethodFailsQuickly()
    {
        await using var engine = EngineProcess.Start();
        await using var client = await engine.ConnectAsync();

        var result = await client.RequestAsync(
            "engine/hello",
            new { name = "clinicavt-shell", version = "0.1.0", protocolVersion = Protocol.ProtocolVersion },
            Timeout);
        var peer = result.Deserialize<PeerInfo>(Protocol.JsonOptions);

        Assert.Equal(ExpectedEngine.Name, peer!.Name);
        Assert.Equal(ExpectedEngine.Version, peer.Version);
        Assert.Equal(Protocol.ProtocolVersion, peer.ProtocolVersion);

        const string clinical = "naïve café-au-lait 東京 µg °C phénoxyméthylpénicilline";
        var echo = await client.RequestAsync("engine/echo", new { payload = clinical }, Timeout);
        Assert.Equal(clinical, echo.GetProperty("payload").GetString());

        var start = System.Diagnostics.Stopwatch.StartNew();
        var error = await Assert.ThrowsAsync<EngineErrorException>(
            () => client.RequestAsync("engine/nonexistent", null, Timeout));
        start.Stop();

        Assert.Equal(-32601, error.Code);
        Assert.True(start.Elapsed < TimeSpan.FromSeconds(1), $"took {start.ElapsedMilliseconds} ms");
    }

    [Fact]
    public async Task EngineDropsAMalformedFrameAndKeepsServing()
    {
        await using var engine = EngineProcess.Start();
        using var raw = new NamedPipeClientStream(
            ".", EngineInfo.PipeName, PipeAccessRights.ReadData | PipeAccessRights.WriteData
                | PipeAccessRights.ReadAttributes | PipeAccessRights.WriteAttributes
                | PipeAccessRights.ReadPermissions,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly,
            System.Security.Principal.TokenImpersonationLevel.Identification,
            HandleInheritability.None);
        await raw.ConnectAsync((int)Timeout.TotalMilliseconds);

        // A well-framed body that is not JSON is dropped and the connection survives
        var junk = "not json at all"u8.ToArray();
        await raw.WriteAsync(BitConverter.GetBytes((uint)junk.Length));
        await raw.WriteAsync(junk);

        // A valid request on the same connection must still be answered
        var request =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"engine/echo\",\"params\":{\"payload\":\"alive\"}}"u8
                .ToArray();
        await raw.WriteAsync(BitConverter.GetBytes((uint)request.Length));
        await raw.WriteAsync(request);
        await raw.FlushAsync();

        using var response = await ReadReplyAsync(raw);

        Assert.Equal(
            "alive",
            response.RootElement.GetProperty("result").GetProperty("payload").GetString());
        Assert.True(engine.IsRunning);
    }

    // The embedder may announce itself on a fresh connection first. The reply carries the id
    private static async Task<JsonDocument> ReadReplyAsync(Stream raw)
    {
        while (true)
        {
            var frame = JsonDocument.Parse(await Framing.ReadFrameAsync(raw));
            if (frame.RootElement.TryGetProperty("id", out _))
            {
                return frame;
            }

            frame.Dispose();
        }
    }

    [Fact]
    public async Task SecondEngineInstanceIsRefusedForClaimingThePipe()
    {
        await using var first = EngineProcess.Start();
        await using var client = await first.ConnectAsync();  // waits until the pipe is up

        await using var second = EngineProcess.Start();
        var exitCode = await second.WaitForExitAsync(Timeout);

        // Its own exit code, so the app takes the first one over rather than counting a crash
        Assert.Equal(3, exitCode);
        Assert.Contains("pipe name already claimed", second.StandardError, StringComparison.Ordinal);
    }
}

[CollectionDefinition("engine", DisableParallelization = true)]
public class EngineTestGroup;
