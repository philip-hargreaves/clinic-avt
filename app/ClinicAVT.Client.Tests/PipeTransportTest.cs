using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using ClinicAVT.Client.Tests.TestDoubles;

namespace ClinicAVT.Client.Tests;

public class PipeTransportTest
{
    private static string UniquePipeName() => $"LOCAL\\clinicavt-test-{Guid.NewGuid():N}";

    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);

    private static NamedPipeServerStream RawServer(string name) => new(
        name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
        PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);

    // One scripted engine, keyed on the method, so a single transport can walk every reply kind
    private static IEnumerable<string> Script(JsonDocument request)
    {
        var id = request.RootElement.GetProperty("id").GetInt64();
        return request.RootElement.GetProperty("method").GetString() switch
        {
            "engine/echo" => [$"{{\"jsonrpc\":\"2.0\",\"id\":{id},\"result\":{{}}}}"],
            "engine/notify" =>
            [
                "{\"jsonrpc\":\"2.0\",\"method\":\"engine/event\",\"params\":{\"n\":1}}",
                $"{{\"jsonrpc\":\"2.0\",\"id\":{id},\"result\":{{}}}}",
            ],
            "engine/nope" =>
            [
                $"{{\"jsonrpc\":\"2.0\",\"id\":{id},\"error\":{{\"code\":-32601,\"message\":\"Method not found\"}}}}",
            ],
            // An id but neither result nor error
            "engine/malformed" => [$"{{\"jsonrpc\":\"2.0\",\"id\":{id}}}"],
            _ => [],  // silence
        };
    }

    [Fact]
    public async Task ATransportChecksThePidThenWalksEveryKindOfReply()
    {
        // The fake server runs in this test process, so a pid it cannot have is refused
        var wrongName = UniquePipeName();
        await using var refusing = new FakeEngine(wrongName, _ => []);
        var wrongPid = (uint)(Environment.ProcessId + 1);
        var refused = await Assert.ThrowsAsync<IOException>(
            () => PipeTransport.ConnectAsync(wrongName, Timeout, wrongPid));
        Assert.Contains("expected engine pid", refused.Message, StringComparison.Ordinal);

        var name = UniquePipeName();
        await using var fake = new FakeEngine(name, Script);
        await using var transport = await PipeTransport.ConnectAsync(
            name, Timeout, (uint)Environment.ProcessId);

        // Non-ASCII goes over the wire as readable UTF-8 without escapes
        const string clinical = "naïve café-au-lait 東京 µg °C";
        var result = await transport.RequestAsync("engine/echo", new { payload = clinical }, Timeout);
        Assert.Equal(JsonValueKind.Object, result.ValueKind);
        Assert.Contains(clinical, fake.LastRequestJson, StringComparison.Ordinal);
        Assert.DoesNotContain("\\u", fake.LastRequestJson, StringComparison.Ordinal);

        // A notification raises its event without disturbing the request it precedes
        var notified = new TaskCompletionSource<string>();
        transport.NotificationReceived += (method, _) => notified.TrySetResult(method);
        await transport.RequestAsync("engine/notify", null, Timeout);
        Assert.Equal("engine/event", await notified.Task.WaitAsync(Timeout));

        var error = await Assert.ThrowsAsync<EngineErrorException>(
            () => transport.RequestAsync("engine/nope", null, Timeout));
        Assert.Equal(-32601, error.Code);

        // A malformed response faults its own request promptly, distinct from a timeout
        await Assert.ThrowsAsync<KeyNotFoundException>(
            () => transport.RequestAsync("engine/malformed", null, Timeout));

        // Silence times out, and the transport is still usable afterwards
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => transport.RequestAsync("engine/silent", null, TimeSpan.FromMilliseconds(200)));
        await transport.RequestAsync("engine/echo", null, Timeout);
    }

    [Fact]
    public async Task ResponsesCorrelateWhenDeliveredOutOfOrder()
    {
        var name = UniquePipeName();
        using var server = RawServer(name);
        var serverTask = Task.Run(async () =>
        {
            await server.WaitForConnectionAsync();
            var ids = new List<long>();
            for (var i = 0; i < 2; i++)
            {
                using var doc = JsonDocument.Parse(await Framing.ReadFrameAsync(server));
                ids.Add(doc.RootElement.GetProperty("id").GetInt64());
            }

            // Answer the second request first
            foreach (var id in Enumerable.Reverse(ids))
            {
                var reply = Encoding.UTF8.GetBytes(
                    $"{{\"jsonrpc\":\"2.0\",\"id\":{id},\"result\":{{\"echoedId\":{id}}}}}");
                await server.WriteAsync(Framing.Encode(reply));
            }

            await server.FlushAsync();
        });
        await using var transport = await PipeTransport.ConnectAsync(name, Timeout);

        var first = transport.RequestAsync("engine/echo", null, Timeout);
        var second = transport.RequestAsync("engine/echo", null, Timeout);
        var results = await Task.WhenAll(first, second);

        Assert.Equal(1, results[0].GetProperty("echoedId").GetInt64());
        Assert.Equal(2, results[1].GetProperty("echoedId").GetInt64());
        await serverTask;
    }

    [Fact]
    public async Task OversizeHeaderFaultsPendingRequests()
    {
        var name = UniquePipeName();
        using var raw = RawServer(name);
        var serverTask = Task.Run(async () =>
        {
            await raw.WaitForConnectionAsync();
            // The reply header declares a body far past the cap
            await Framing.ReadFrameAsync(raw);
            await raw.WriteAsync(BitConverter.GetBytes(uint.MaxValue));
            await raw.FlushAsync();
        });
        await using var transport = await PipeTransport.ConnectAsync(name, Timeout);

        await Assert.ThrowsAsync<InvalidDataException>(
            () => transport.RequestAsync("engine/echo", null, Timeout));
        await serverTask;
    }
}
