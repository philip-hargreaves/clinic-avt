using System.Diagnostics;
using System.Text;

namespace ClinicAVT.Client.Tests.Support;

/// <summary>
/// Launches the real clinicavt_engine.exe and connects a verified client to it.
/// </summary>
internal sealed class EngineProcess : IAsyncDisposable
{
    private const string DefaultPipeName = EngineInfo.PipeName;
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(10);

    // A note model load cannot be cancelled, and the engine stays until it finishes
    private static readonly TimeSpan ExitWait = TimeSpan.FromSeconds(60);

    private readonly Process _process;
    private readonly string _pipeName;
    private readonly StringBuilder _stderr = new();

    private EngineProcess(Process process, string pipeName, string storeRoot)
    {
        _process = process;
        _pipeName = pipeName;
        StoreRoot = storeRoot;
        // An undrained stderr pipe blocks the engine
        _process.ErrorDataReceived += (_, e) =>
        {
            if (e.Data is not null)
            {
                lock (_stderr)
                {
                    _stderr.AppendLine(e.Data);
                }
            }
        };
        _process.BeginErrorReadLine();
    }

    // Private pipe and roots per run so tests leave the app's alone. A replay
    // wav stands in for the microphone. Scripted, a model that is not installed
    // gets a stand-in; without it the engine refuses consultations as the app's does
    public static EngineProcess Start(
        string? pipeName = null, string? replayWavPath = null, string? modelsRoot = null,
        bool scripted = true, bool allowReplay = true)
    {
        var storeRoot = Path.Combine(Path.GetTempPath(), $"clinicavt-store-{Guid.NewGuid():N}");
        var startInfo = new ProcessStartInfo(EnginePath.Find())
        {
            UseShellExecute = false,
            RedirectStandardError = true,
        };
        if (scripted)
        {
            startInfo.ArgumentList.Add("--scripted");
        }

        if (allowReplay)
        {
            startInfo.ArgumentList.Add("--allow-replay");
        }

        startInfo.ArgumentList.Add(pipeName ?? DefaultPipeName);
        startInfo.ArgumentList.Add(storeRoot);
        startInfo.ArgumentList.Add(
            modelsRoot ?? Path.Combine(Path.GetTempPath(), $"clinicavt-models-{Guid.NewGuid():N}"));
        if (replayWavPath is not null)
        {
            startInfo.ArgumentList.Add(replayWavPath);
        }

        var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException("engine failed to start");
        return new EngineProcess(process, pipeName ?? DefaultPipeName, storeRoot);
    }

    public bool IsRunning => !_process.HasExited;

    public string StoreRoot { get; }

    public string StandardError
    {
        get
        {
            lock (_stderr)
            {
                return _stderr.ToString();
            }
        }
    }

    public async Task<int> WaitForExitAsync(TimeSpan timeout)
    {
        using var cts = new CancellationTokenSource(timeout);
        await _process.WaitForExitAsync(cts.Token).ConfigureAwait(false);
        return _process.ExitCode;
    }

    public Task<PipeTransport> ConnectAsync()
    {
        if (_process.HasExited)
        {
            throw new InvalidOperationException(
                $"engine exited (code {_process.ExitCode}) before a client could connect: {StandardError}");
        }

        return PipeTransport.ConnectAsync(_pipeName, ConnectTimeout, (uint)_process.Id);
    }

    // Asks the engine to leave and waits, never kills: an engine stopped mid-GPU can wedge
    // the driver. One that will not leave fails the test and is left running
    public async ValueTask DisposeAsync()
    {
        if (!_process.HasExited)
        {
            var asked = await AskToExitAsync().ConfigureAwait(false);
            try
            {
                await WaitForExitAsync(ExitWait).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                throw new InvalidOperationException(
                    $"engine pid {_process.Id} did not leave within {ExitWait.TotalSeconds:0} s "
                    + $"({asked}) and was left running: {StandardError}");
            }
        }

        _process.Dispose();
        try
        {
            Directory.Delete(StoreRoot, recursive: true);
        }
        catch (IOException)
        {
            // A leftover temp store is harmless
        }
    }

    // The engine leaves once asked and alone, so the request goes on a connection of its own
    // that closes straight after. The outcome goes into the failure message if it stays
    private async Task<string> AskToExitAsync()
    {
        try
        {
            await using var client = await ConnectAsync().ConfigureAwait(false);
            await client.RequestAsync("engine/exit", null, ConnectTimeout).ConfigureAwait(false);
            return "asked with engine/exit";
        }
        catch (Exception e)
        {
            // It may have left on its own. The wait decides
            return $"engine/exit not delivered: {e.Message}";
        }
    }
}
