using System.Text.Json;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;
using static ClinicAVT.App.Tests.Support.Waits;

namespace ClinicAVT.App.Tests.Hosting;

public class EngineConnectionTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);

    private static readonly JsonElement Empty = JsonSerializer.SerializeToElement(new { });

    private sealed class FakeTransport : IEngineTransport
    {
        public int HelloProtocol { get; init; } = Protocol.ProtocolVersion;

        public List<string> Requests { get; } = [];

        public bool Disposed { get; private set; }

        public TaskCompletionSource<JsonElement>? PendingResponse { get; set; }

        public event Action<string, JsonElement>? NotificationReceived;

        public event Action<bool>? ConnectedChanged
        {
            add { }
            remove { }
        }

        public bool Connected => true;

        public Task<JsonElement> RequestAsync(
            string method, object? parameters, TimeSpan timeout,
            CancellationToken cancellationToken = default)
        {
            Requests.Add(method);
            if (method == "engine/hello")
            {
                return Task.FromResult(JsonSerializer.SerializeToElement(
                    new PeerInfo(ExpectedEngine.Name, ExpectedEngine.Version, HelloProtocol),
                    Protocol.JsonOptions));
            }

            return PendingResponse?.Task ?? Task.FromResult(Empty);
        }

        public void RaiseNotification(string method) =>
            NotificationReceived?.Invoke(method, Empty);

        public ValueTask DisposeAsync()
        {
            Disposed = true;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class Rig
    {
        public FakeEngineHost Host { get; } = new() { EnginePid = 4321 };

        public List<FakeTransport> Transports { get; } = [];

        public List<uint> ConnectPids { get; } = [];

        public int HelloProtocol { get; set; } = Protocol.ProtocolVersion;

        public Exception? ConnectFailure { get; set; }

        public TaskCompletionSource<IEngineTransport>? PendingConnect { get; set; }

        public EngineConnection Connection { get; }

        public Rig()
        {
            Connection = new EngineConnection(Host, (pid, _) =>
            {
                ConnectPids.Add(pid);
                if (ConnectFailure is not null)
                {
                    throw ConnectFailure;
                }

                if (PendingConnect is { } pending)
                {
                    PendingConnect = null;
                    return pending.Task;
                }

                var transport = new FakeTransport { HelloProtocol = HelloProtocol };
                Transports.Add(transport);
                return Task.FromResult<IEngineTransport>(transport);
            });
        }
    }

    [Fact]
    public async Task AConnectionComesUpTracksItsRequestAndSurvivesARestart()
    {
        var rig = new Rig();
        string? seen = null;
        rig.Connection.NotificationReceived += (method, _) => seen = method;

        // Fail fast until the engine is up
        await Assert.ThrowsAsync<IOException>(
            () => rig.Connection.RequestAsync("session/start", null, Timeout));

        rig.Host.RaiseStatus(EngineStatus.Running);
        Assert.Equal(4321u, Assert.Single(rig.ConnectPids));
        var transport = Assert.Single(rig.Transports);
        Assert.Equal("engine/hello", Assert.Single(transport.Requests));

        await rig.Connection.RequestAsync("session/start", null, Timeout);
        Assert.Equal("session/start", transport.Requests[^1]);

        transport.RaiseNotification("note/ready");
        Assert.Equal("note/ready", seen);

        // The outstanding method is visible while it is in flight
        transport.PendingResponse = new TaskCompletionSource<JsonElement>();
        var request = rig.Connection.RequestAsync("session/stop", null, Timeout);
        Assert.Equal("session/stop", rig.Connection.MethodInFlight);
        transport.PendingResponse.SetResult(Empty);
        await request;
        Assert.Null(rig.Connection.MethodInFlight);

        // A restart drops the old transport and dials a new one
        rig.Host.RaiseStatus(EngineStatus.Restarting);
        Assert.True(transport.Disposed);
        await Assert.ThrowsAsync<IOException>(
            () => rig.Connection.RequestAsync("session/start", null, Timeout));

        rig.Host.RaiseStatus(EngineStatus.Running);
        Assert.Equal(2, rig.Transports.Count);
        await rig.Connection.RequestAsync("session/start", null, Timeout);
        Assert.Equal("session/start", rig.Transports[1].Requests[^1]);
    }

    [Fact]
    public async Task AProtocolMismatchOrAFailedConnectSurfacesOnRequests()
    {
        var mismatched = new Rig { HelloProtocol = 99 };
        mismatched.Host.RaiseStatus(EngineStatus.Running);
        Assert.True(mismatched.Transports[0].Disposed);
        var thrown = await Assert.ThrowsAsync<IOException>(
            () => mismatched.Connection.RequestAsync("session/start", null, Timeout));
        Assert.Contains("protocol", thrown.InnerException!.Message, StringComparison.Ordinal);

        var unreachable = new Rig { ConnectFailure = new IOException("pipe never appeared") };
        unreachable.Host.RaiseStatus(EngineStatus.Running);
        thrown = await Assert.ThrowsAsync<IOException>(
            () => unreachable.Connection.RequestAsync("session/start", null, Timeout));
        Assert.Equal("pipe never appeared", thrown.InnerException!.Message);
    }

    [Fact]
    public async Task AStaleConnectDoesNotClobberANewerOne()
    {
        var rig = new Rig();
        var pending = new TaskCompletionSource<IEngineTransport>();
        rig.PendingConnect = pending;
        rig.Host.RaiseStatus(EngineStatus.Running);

        rig.Host.RaiseStatus(EngineStatus.Restarting);
        rig.Host.RaiseStatus(EngineStatus.Running);
        var current = rig.Transports[^1];

        var stale = new FakeTransport();
        pending.SetResult(stale);

        await WaitUntilAsync(() => stale.Disposed);
        await rig.Connection.RequestAsync("session/start", null, Timeout);
        Assert.Equal("session/start", current.Requests[^1]);
        Assert.DoesNotContain("session/start", stale.Requests);
    }
}
