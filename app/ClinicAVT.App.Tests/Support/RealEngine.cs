using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Platform;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;
using static ClinicAVT.App.Tests.Support.Waits;

namespace ClinicAVT.App.Tests.Support;

/// <summary>
/// The shell's own stack over the real engine: launcher, supervisor and connection, on a private
/// pipe with a temp store. It answers echo by the time StartAsync returns.
/// </summary>
internal sealed class RealEngine : IAsyncDisposable
{
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(15);

    private readonly ProcessEngineLauncher _launcher;

    private RealEngine(ProcessEngineLauncher launcher, string pipeName, string directory)
    {
        _launcher = launcher;
        Directory = directory;
        Host = new EngineSupervisor(
            launcher, new FakeSession(), TimeProvider.System, new FileCrashLog(CrashLog));
        Connection = new EngineConnection(Host, async (pid, ct) =>
            await PipeTransport.ConnectAsync(pipeName, ConnectTimeout, pid, ct));
    }

    public EngineSupervisor Host { get; }

    public EngineConnection Connection { get; }

    public string Directory { get; }

    public string CrashLog => Path.Combine(Directory, "crashes.jsonl");

    /// <summary>
    /// With models, the staged ones when present, so startup cost and code paths match the
    /// shipped app. Without any, as in CI, stand-ins take their place. Tests replay wav files
    /// as the microphone. A stderr name keeps the engine's log in the temp folder.
    /// </summary>
    public static async Task<RealEngine> StartAsync(string tag, bool models = true, string? stderr = null)
    {
        var pipeName = $"LOCAL\\clinicavt-{tag}-{Guid.NewGuid():N}";
        var directory = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        System.IO.Directory.CreateDirectory(directory);
        var modelsRoot = models ? EnginePath.FindModels() : null;
        var launcher = new ProcessEngineLauncher(
            EnginePath.Find(),
            (modelsRoot is null ? "--scripted " : "") + "--allow-replay "
                + $"{pipeName} \"{Path.Combine(directory, "store")}\""
                + (modelsRoot is null ? "" : $" \"{modelsRoot}\""),
            stderrPath: stderr is null ? null : Path.Combine(Path.GetTempPath(), stderr));
        var engine = new RealEngine(launcher, pipeName, directory);
        try
        {
            engine.Host.Start();
            await RetryAsync(() => engine.Connection.RequestAsync(
                "engine/echo", new { payload = "up" }, ConnectTimeout));
            return engine;
        }
        catch (IOException e)
        {
            var crashes = File.Exists(engine.CrashLog) ? File.ReadAllText(engine.CrashLog) : "<none>";
            var status = engine.Host.Status;
            var pid = engine.Host.EnginePid;
            await engine.DisposeAsync();
            throw new IOException($"echo never connected. status={status} pid={pid} crashes={crashes}", e);
        }
        catch (Exception)
        {
            await engine.DisposeAsync();
            throw;
        }
    }

    public async ValueTask DisposeAsync()
    {
        Host.Dispose();
        await Connection.DisposeAsync();
        _launcher.Dispose();
        try
        {
            System.IO.Directory.Delete(Directory, recursive: true);
        }
        catch (IOException)
        {
            // The engine may still hold the store for a moment, and temp cleans itself
        }
    }
}
