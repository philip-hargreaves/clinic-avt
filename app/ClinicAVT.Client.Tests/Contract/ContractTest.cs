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

    // The app starts the engine without --scripted. A model that is not installed is then named
    // and refuses a consultation, where CI's scripted engine gives it a stand-in
    [Fact]
    public async Task WithoutScriptedAMissingModelIsNamedAndRefusesAConsultation()
    {
        await using var engine = EngineProcess.Start(
            $"LOCAL\\clinicavt-unscripted-{Guid.NewGuid():N}", scripted: false, allowReplay: false);
        await using var client = await engine.ConnectAsync();
        var api = new EngineApi(client);

        var readiness = await api.ReadinessAsync();
        Assert.Equal(["asr", "vad", "diarisation", "segmentation"], readiness.Missing);

        const string reason =
            "the speech recognition, speech detection and speaker recognition models are not installed";
        var start = await Assert.ThrowsAsync<EngineErrorException>(() => api.StartSessionAsync(true, ""));
        Assert.Equal(reason, start.Message);
        var enrol = await Assert.ThrowsAsync<EngineErrorException>(
            () => client.RequestAsync("anchor/enrol", new { seconds = 1.0 }, Timeout));
        Assert.Equal(reason, enrol.Message);
        Assert.Empty(await api.ListSessionsAsync());
        Assert.True(engine.IsRunning, "it keeps serving, so the app can say why");
    }

    // A replay reads whatever file it names, so only an engine started to allow it takes one
    [Fact]
    public async Task ReplayNeedsAnEngineStartedToAllowIt()
    {
        var wav = SilenceWav.Write();
        try
        {
            await using (var launchWav = EngineProcess.Start(
                             $"LOCAL\\clinicavt-noreplay-{Guid.NewGuid():N}", wav, allowReplay: false))
            {
                Assert.Equal(1, await launchWav.WaitForExitAsync(Timeout));
                Assert.Contains("--allow-replay", launchWav.StandardError, StringComparison.Ordinal);
            }

            await using var engine = EngineProcess.Start(
                $"LOCAL\\clinicavt-noreplay-{Guid.NewGuid():N}", allowReplay: false);
            await using var client = await engine.ConnectAsync();
            var error = await Assert.ThrowsAsync<EngineErrorException>(() => client.RequestAsync(
                "session/start", new { replay = new { path = wav, speed = 16.0 } }, Timeout));
            Assert.Equal(-32602, error.Code);
            Assert.Empty(await new EngineApi(client).ListSessionsAsync());
        }
        finally
        {
            File.Delete(wav);
        }
    }

    // The name reaches the engine as UTF-16 and must be served unchanged
    [Fact]
    public async Task APipeNameOutsideAsciiIsServedUnderThatName()
    {
        await using var engine = EngineProcess.Start($"LOCAL\\clinicavt-café-東京-{Guid.NewGuid():N}");
        await using var client = await engine.ConnectAsync();

        var echo = await client.RequestAsync("engine/echo", new { payload = "up" }, Timeout);
        Assert.Equal("up", echo.GetProperty("payload").GetString());
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
