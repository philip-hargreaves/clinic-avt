using System.Text.Json;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Hosting;

/// <summary>
/// An IEngineTransport that follows the supervisor. It connects when the engine comes up,
/// drops the transport when it goes down, and fails requests fast in between. Each engine
/// process gets its own pid-verified connection.
/// </summary>
public sealed class EngineConnection : IEngineTransport
{
    private const string ShellName = "clinicavt-shell";
    private static readonly string ShellVersion =
        typeof(EngineConnection).Assembly.GetName().Version!.ToString(3);

    // The engine binds its pipe before model verification and compilation, and
    // answers the buffered hello only once those finish
    private static readonly TimeSpan HelloTimeout = TimeSpan.FromSeconds(120);

    private static readonly TimeSpan RedialDelay = TimeSpan.FromMilliseconds(500);

    private readonly IEngineHost _host;
    private readonly Func<uint, CancellationToken, Task<IEngineTransport>> _connect;
    private readonly ILogger? _logger;
    private readonly CancellationTokenSource _disposal = new();
    private readonly object _gate = new();
    private IEngineTransport? _transport;
    private Exception? _lastConnectError;
    private int _generation;
    // Cancels the dial of a generation that has ended, so a stale one can never take the
    // next engine's pipe
    private CancellationTokenSource? _dial;
    private volatile string? _methodInFlight;
    private int _disposed;

    public EngineConnection(
        IEngineHost host, Func<uint, CancellationToken, Task<IEngineTransport>> connect,
        ILogger? logger = null)
    {
        _host = host;
        _connect = connect;
        _logger = logger;
        host.StatusChanged += OnEngineStatusChanged;
        if (host.Status == EngineStatus.Running)
        {
            OnEngineStatusChanged(EngineStatus.Running);
        }
    }

    public event Action<string, JsonElement>? NotificationReceived;

    public event Action<bool>? ConnectedChanged;

    public bool Connected
    {
        get
        {
            lock (_gate)
            {
                return _transport is not null;
            }
        }
    }

    /// <summary>The request outstanding right now, for the crash report.</summary>
    public string? MethodInFlight => _methodInFlight;

    public async Task<JsonElement> RequestAsync(
        string method, object? parameters, TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        IEngineTransport transport;
        lock (_gate)
        {
            transport = _transport
                ?? throw new IOException("engine is not connected", _lastConnectError);
        }

        _methodInFlight = method;
        try
        {
            return await transport.RequestAsync(method, parameters, timeout, cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            _methodInFlight = null;
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
        {
            return;
        }

        _host.StatusChanged -= OnEngineStatusChanged;
        await _disposal.CancelAsync().ConfigureAwait(false);
        IEngineTransport? transport;
        lock (_gate)
        {
            transport = _transport;
            _transport = null;
        }

        if (transport is not null)
        {
            await transport.DisposeAsync().ConfigureAwait(false);
        }

        _disposal.Dispose();
    }

    private void OnEngineStatusChanged(EngineStatus status)
    {
        if (status == EngineStatus.Running)
        {
            int generation;
            CancellationToken dial;
            lock (_gate)
            {
                generation = ++_generation;
                _dial?.Cancel();
                _dial = CancellationTokenSource.CreateLinkedTokenSource(_disposal.Token);
                dial = _dial.Token;
            }

            _ = ConnectAsync(generation, dial);
        }
        else
        {
            DropTransport();
        }
    }

    private void DropTransport()
    {
        IEngineTransport? old;
        lock (_gate)
        {
            _generation++;
            _dial?.Cancel();
            _dial = null;
            old = _transport;
            _transport = null;
        }

        if (old is not null)
        {
            Observe(old.DisposeAsync().AsTask(), "transport dispose");
            ConnectedChanged?.Invoke(false);
        }
    }

    private async Task ConnectAsync(int generation, CancellationToken dial)
    {
        // Redials until installed or superseded. Giving up would leave the connection dead for
        // good
        while (Volatile.Read(ref _disposed) == 0 && !dial.IsCancellationRequested)
        {
            lock (_gate)
            {
                if (_generation != generation)
                {
                    return;
                }
            }

            try
            {
                if (await TryConnectOnceAsync(generation, dial).ConfigureAwait(false))
                {
                    return;
                }
            }
            catch (OperationCanceledException) when (dial.IsCancellationRequested)
            {
                return;
            }
            catch (Exception e)
            {
                _logger?.StepFailed("engine connect attempt", e.Message);
                lock (_gate)
                {
                    if (_generation == generation)
                    {
                        _lastConnectError = e;
                    }
                }
            }

            try
            {
                await Task.Delay(RedialDelay, dial).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }

    // True to stop dialling, once installed or when a newer generation owns the connection
    private async Task<bool> TryConnectOnceAsync(int generation, CancellationToken dial)
    {
        var pid = _host.EnginePid ?? throw new IOException("engine pid unavailable");
        var transport = await _connect((uint)pid, dial).ConfigureAwait(false);
        var installed = false;
        try
        {
            var hello = await transport.RequestAsync(
                "engine/hello", new PeerInfo(ShellName, ShellVersion, Protocol.ProtocolVersion),
                HelloTimeout, dial).ConfigureAwait(false);
            var engineProtocol = hello.GetProperty("protocolVersion").GetInt32();
            if (engineProtocol != Protocol.ProtocolVersion)
            {
                throw new IOException(FormattableString.Invariant(
                    $"engine speaks protocol {engineProtocol}, this shell needs {Protocol.ProtocolVersion}"));
            }

            transport.NotificationReceived += OnInnerNotification;
            lock (_gate)
            {
                // A newer status event may own the connection by now
                if (_generation == generation && _disposed == 0)
                {
                    _transport = transport;
                    _lastConnectError = null;
                    installed = true;
                }
            }

            if (installed)
            {
                ConnectedChanged?.Invoke(true);
            }

            return true;
        }
        finally
        {
            if (!installed)
            {
                await transport.DisposeAsync().ConfigureAwait(false);
            }
        }
    }

    private void OnInnerNotification(string method, JsonElement parameters) =>
        NotificationReceived?.Invoke(method, parameters);

    // Fire-and-forget work still reports a failure
    private void Observe(Task task, string what) =>
        _ = task.ContinueWith(
            t => _logger?.StepFailed(what, t.Exception?.GetBaseException().Message ?? "faulted"),
            CancellationToken.None, TaskContinuationOptions.OnlyOnFaulted, TaskScheduler.Default);
}
