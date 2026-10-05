using System.Collections.Concurrent;
using System.IO.Pipes;
using System.Runtime.Versioning;
using System.Security.Principal;
using System.Text.Json;

namespace ClinicAVT.Client;

/// <summary>
/// JSON-RPC client over the engine's named pipe. It uses one connection, matches concurrent
/// requests by id and raises notifications as an event. Any transport failure is terminal and
/// fails pending and future requests. This is the client's only Windows-specific type, because
/// pipe access rights and the server pid check are Win32.
/// </summary>
[SupportedOSPlatform("windows")]
public sealed class PipeTransport : IEngineTransport
{
    private readonly NamedPipeClientStream _pipe;
    private readonly ConcurrentDictionary<long, TaskCompletionSource<JsonElement>> _pending = new();
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private readonly CancellationTokenSource _closed = new();
    private Task _readLoop = Task.CompletedTask;
    private long _nextId;
    private Exception? _fault;
    private int _disposed;

    public event Action<string, JsonElement>? NotificationReceived;

    // True until the transport faults. EngineConnection raises ConnectedChanged, so the
    // event below is empty
    public bool Connected => Volatile.Read(ref _fault) is null && Volatile.Read(ref _disposed) == 0;

    public event Action<bool>? ConnectedChanged
    {
        add { }
        remove { }
    }

    private PipeTransport(NamedPipeClientStream pipe) => _pipe = pipe;

    public static async Task<PipeTransport> ConnectAsync(
        string pipeName, TimeSpan timeout, uint? expectedServerProcessId = null,
        CancellationToken cancellationToken = default)
    {
        const PipeAccessRights rights = PipeAccessRights.ReadData | PipeAccessRights.WriteData
            | PipeAccessRights.ReadAttributes | PipeAccessRights.WriteAttributes
            | PipeAccessRights.ReadPermissions;
        var pipe = new NamedPipeClientStream(
            ".", pipeName, rights,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly,
            TokenImpersonationLevel.Identification, HandleInheritability.None);
        try
        {
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            cts.CancelAfter(timeout);
            await pipe.ConnectAsync(cts.Token).ConfigureAwait(false);

            // Refuse any other process that claimed the pipe name first
            if (expectedServerProcessId is uint expected)
            {
                var actual = ServerVerifier.GetServerProcessId(pipe);
                if (actual != expected)
                {
                    var actualText = actual?.ToString(System.Globalization.CultureInfo.InvariantCulture)
                        ?? "<unavailable>";
                    throw new IOException(FormattableString.Invariant(
                        $"pipe server pid {actualText} is not the expected engine pid {expected}"));
                }
            }
        }
        catch
        {
            await pipe.DisposeAsync().ConfigureAwait(false);
            throw;
        }

        var transport = new PipeTransport(pipe);
        transport._readLoop = transport.ReadLoopAsync();
        return transport;
    }

    /// <summary>Pid serving the pipe, found by connecting and closing at once (the engine drops a
    /// client that sends nothing). Null on timeout.</summary>
    public static uint? ServingProcessId(string pipeName, TimeSpan timeout)
    {
        using var pipe = new NamedPipeClientStream(
            ".", pipeName, PipeDirection.InOut, PipeOptions.CurrentUserOnly);
        try
        {
            pipe.Connect(timeout);
        }
        catch (Exception e) when (e is TimeoutException or IOException or UnauthorizedAccessException)
        {
            return null;
        }

        return ServerVerifier.GetServerProcessId(pipe);
    }

    public async Task<JsonElement> RequestAsync(
        string method, object? parameters, TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        if (Volatile.Read(ref _fault) is { } fault)
        {
            throw new IOException("pipe transport is closed", fault);
        }

        var id = Interlocked.Increment(ref _nextId);
        var completion = new TaskCompletionSource<JsonElement>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = completion;

        // A fault between the check above and the insert did not see this completion
        if (Volatile.Read(ref _fault) is { } raced)
        {
            _pending.TryRemove(id, out _);
            throw new IOException("pipe transport is closed", raced);
        }

        using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        cts.CancelAfter(timeout);
        try
        {
            var request = new Dictionary<string, object?>
            {
                ["jsonrpc"] = "2.0",
                ["id"] = id,
                ["method"] = method,
            };
            if (parameters is not null)
            {
                request["params"] = parameters;
            }

            await SendAsync(request, cts.Token).ConfigureAwait(false);
            return await completion.Task.WaitAsync(cts.Token).ConfigureAwait(false);
        }
        finally
        {
            _pending.TryRemove(id, out _);
        }
    }

    private async Task SendAsync(object message, CancellationToken cancellationToken)
    {
        var frame = Framing.Encode(JsonSerializer.SerializeToUtf8Bytes(message, Protocol.JsonOptions));
        // A cancelled write desyncs the stream, so any write failure is terminal
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken, _closed.Token);
        await _writeLock.WaitAsync(linked.Token).ConfigureAwait(false);
        try
        {
            await _pipe.WriteAsync(frame, linked.Token).ConfigureAwait(false);
            await _pipe.FlushAsync(linked.Token).ConfigureAwait(false);
        }
        catch (Exception e)
        {
            Fault(e);
            // A write cancelled by closure reports what caused the closure
            if (Volatile.Read(ref _fault) is { } cause && !ReferenceEquals(cause, e))
            {
                throw new IOException("pipe transport is closed", cause);
            }

            throw;
        }
        finally
        {
            _writeLock.Release();
        }
    }

    private async Task ReadLoopAsync()
    {
        try
        {
            while (true)
            {
                var body = await Framing.ReadFrameAsync(_pipe, _closed.Token).ConfigureAwait(false);
                using var document = JsonDocument.Parse(body);
                Dispatch(document.RootElement);
            }
        }
        catch (Exception e)
        {
            Fault(e);
        }
    }

    private void Dispatch(JsonElement root)
    {
        if (!root.TryGetProperty("id", out var idElement))
        {
            if (root.TryGetProperty("method", out var method))
            {
                var parameters = root.TryGetProperty("params", out var p) ? p.Clone() : default;
                try
                {
                    NotificationReceived?.Invoke(method.GetString() ?? "", parameters);
                }
                catch
                {
                    // A subscriber's failure is not a transport failure
                }
            }

            return;
        }

        if (!idElement.TryGetInt64(out var id) || !_pending.TryRemove(id, out var completion))
        {
            return;
        }

        // A malformed response faults only its own request. The completion is already removed
        try
        {
            if (root.TryGetProperty("error", out var error))
            {
                var data = error.TryGetProperty("data", out var d) ? d.Clone() : (JsonElement?)null;
                completion.SetException(new EngineErrorException(
                    error.GetProperty("code").GetInt32(),
                    error.GetProperty("message").GetString() ?? "", data));
            }
            else
            {
                completion.SetResult(root.GetProperty("result").Clone());
            }
        }
        catch (Exception e)
        {
            completion.TrySetException(e);
        }
    }

    private void Fault(Exception cause)
    {
        // Only the first fault is kept
        if (Interlocked.CompareExchange(ref _fault, cause, null) is not null)
        {
            return;
        }

        // Pending requests fail with the real cause before the cancellation below reaches them
        foreach (var id in _pending.Keys)
        {
            if (_pending.TryRemove(id, out var completion))
            {
                completion.TrySetException(cause);
            }
        }

        _closed.Cancel();
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
        {
            return;
        }

        Fault(new ObjectDisposedException(nameof(PipeTransport)));
        await _pipe.DisposeAsync().ConfigureAwait(false);
        try
        {
            await _readLoop.ConfigureAwait(false);
        }
        catch
        {
            // The loop's exit already faulted every pending request
        }

        // _writeLock and _closed stay undisposed. A request parked on either must
        // see the clean close, and neither holds an unmanaged handle
    }
}
