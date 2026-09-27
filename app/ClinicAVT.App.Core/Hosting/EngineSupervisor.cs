using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Hosting;

/// <summary>
/// Runs the engine and applies RestartPolicy when it dies unexpectedly. It never kills the
/// engine: releasing it asks it to leave. Status events are raised outside the lock, after the
/// state has settled.
/// </summary>
public sealed class EngineSupervisor(
    IEngineLauncher launcher, ISessionState session, TimeProvider clock, ICrashLog crashLog,
    Func<string?>? methodInFlight = null, Func<Task>? askToExit = null)
    : IEngineHost, IDisposable
{
    /// <summary>The engine's exit code when another engine already serves the pipe.</summary>
    public const int AlreadyServing = 3;

    private readonly object _gate = new();
    private readonly List<DateTimeOffset> _crashes = [];
    private IEngineProcess? _process;
    private ITimer? _relaunch;
    private DateTimeOffset _launchedAt;

    public event Action<EngineStatus>? StatusChanged;

    public EngineStatus Status { get; private set; } = EngineStatus.Stopped;

    public int? EnginePid
    {
        get
        {
            lock (_gate)
            {
                return _process?.Id;
            }
        }
    }

    public void Start()
    {
        var changes = new List<EngineStatus>();
        lock (_gate)
        {
            if (_process is not null)
            {
                return;
            }

            CancelRelaunchLocked();
            _crashes.Clear();
            LaunchLocked(changes);
        }

        Raise(changes);
    }

    public async Task ReleaseAsync()
    {
        var process = await LetGoAsync().ConfigureAwait(false);
        launcher.Release();
        process?.Dispose();
    }

    // Stops watching. Whatever owns the launcher decides whether the engine outlives it
    public void Dispose() => Detach()?.Dispose();

    // Asks while the connection is still up. Stopped then drops it, and the
    // engine, alone, leaves
    private async Task<IEngineProcess?> LetGoAsync()
    {
        if (askToExit is not null && Status == EngineStatus.Running)
        {
            try
            {
                await askToExit().ConfigureAwait(false);
            }
            catch (Exception)
            {
                // An engine that cannot be asked still leaves once idle and alone
            }
        }

        return Detach();
    }

    private IEngineProcess? Detach()
    {
        IEngineProcess? process;
        var changes = new List<EngineStatus>();
        lock (_gate)
        {
            if (Status == EngineStatus.Stopped)
            {
                return null;
            }

            CancelRelaunchLocked();
            process = _process;
            _process = null;
            SetStatusLocked(EngineStatus.Stopped, changes);
        }

        Raise(changes);
        return process;
    }

    private void LaunchLocked(List<EngineStatus> changes)
    {
        IEngineProcess process;
        try
        {
            process = launcher.Launch();
        }
        catch (Exception)
        {
            SetStatusLocked(EngineStatus.Faulted, changes);
            return;
        }

        _process = process;
        _launchedAt = clock.GetUtcNow();
        process.Exited += () => OnExited(process);
        SetStatusLocked(EngineStatus.Running, changes);

        // It may have died before the handler was attached
        if (process.HasExited)
        {
            HandleExitLocked(process, changes);
        }
    }

    private void OnExited(IEngineProcess process)
    {
        var changes = new List<EngineStatus>();
        lock (_gate)
        {
            HandleExitLocked(process, changes);
        }

        Raise(changes);
    }

    private void HandleExitLocked(IEngineProcess process, List<EngineStatus> changes)
    {
        // A stale event. A deliberate stop or another path has handled it
        if (!ReferenceEquals(process, _process))
        {
            return;
        }

        _process = null;
        var exitCode = process.ExitCode;
        process.Dispose();

        // An engine a closed app left to finish a load still serves the pipe. It is taken
        // over rather than counted as a crash
        if (exitCode == AlreadyServing && launcher.Adopt() is { } running)
        {
            _process = running;
            _launchedAt = clock.GetUtcNow();
            running.Exited += () => OnExited(running);
            SetStatusLocked(EngineStatus.Running, changes);
            if (running.HasExited)
            {
                HandleExitLocked(running, changes);
            }

            return;
        }

        var now = clock.GetUtcNow();
        _crashes.RemoveAll(crash => now - crash > RestartPolicy.StormWindow);
        _crashes.Add(now);

        var action = RestartPolicy.Decide(_crashes, now);
        crashLog.Record(new CrashReport(
            now, exitCode, now - _launchedAt, _crashes.Count, action, methodInFlight?.Invoke(),
            session.SessionPhase));

        if (action == RecoveryAction.GiveUp)
        {
            SetStatusLocked(EngineStatus.Faulted, changes);
            return;
        }

        SetStatusLocked(EngineStatus.Restarting, changes);
        var wait = RestartPolicy.Backoff(_crashes.Count);
        if (wait == TimeSpan.Zero)
        {
            LaunchLocked(changes);
            return;
        }

        CancelRelaunchLocked();
        _relaunch = clock.CreateTimer(_ => Relaunch(), null, wait, Timeout.InfiniteTimeSpan);
    }

    // Runs when the backoff elapses. It does nothing if a Start or Shutdown got there first
    private void Relaunch()
    {
        var changes = new List<EngineStatus>();
        lock (_gate)
        {
            if (Status != EngineStatus.Restarting || _process is not null)
            {
                return;
            }

            LaunchLocked(changes);
        }

        Raise(changes);
    }

    private void CancelRelaunchLocked()
    {
        _relaunch?.Dispose();
        _relaunch = null;
    }

    private void SetStatusLocked(EngineStatus status, List<EngineStatus> changes)
    {
        Status = status;
        changes.Add(status);
    }

    private void Raise(List<EngineStatus> changes)
    {
        foreach (var status in changes)
        {
            StatusChanged?.Invoke(status);
        }
    }
}
