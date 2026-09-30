using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Hosting;

/// <summary>
/// Runs the engine and applies RestartPolicy on an unexpected exit. It never kills the engine.
/// Release asks it to exit. Status events are raised outside the lock, after the state settles.
/// </summary>
public sealed class EngineSupervisor(
    IEngineLauncher launcher, ISessionState session, TimeProvider clock, ICrashLog crashLog,
    Func<string?>? methodInFlight = null, Func<Task>? askToExit = null)
    : IEngineHost, IDisposable
{
    /// <summary>The engine's exit code when another engine already serves the pipe.</summary>
    public const int AlreadyServing = 3;

    /// <summary>The engine's exit code when the store was written by a newer version.</summary>
    public const int StoreNewer = 4;

    /// <summary>The engine's exit code when the store is too old to upgrade.</summary>
    public const int StoreTooOld = 5;

    /// <summary>The retry interval while another engine holds the pipe.</summary>
    public static readonly TimeSpan ServerWait = TimeSpan.FromSeconds(2);

    /// <summary>How long to wait for that engine before its exits count as crashes.</summary>
    public static readonly TimeSpan ServerWaitLimit = TimeSpan.FromMinutes(10);

    private readonly object _gate = new();
    private readonly List<DateTimeOffset> _crashes = [];
    private IEngineProcess? _process;
    private ITimer? _relaunch;
    private DateTimeOffset _launchedAt;
    private DateTimeOffset? _waitingSince;
    private DateTimeOffset _lastWait;

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
            _waitingSince = null;
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

    // This only stops watching. The launcher's owner decides whether the engine keeps running
    public void Dispose() => Detach()?.Dispose();

    // Ask before the connection drops. The engine exits on its own once it has no clients
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
                // Ignored, because the engine still exits once idle with no clients
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

        // An engine left running by a closed app may still be finishing a load and own the pipe. It
        // is adopted and does not count as a crash
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
        // Every launch would fail the same way
        if (exitCode is StoreNewer or StoreTooOld)
        {
            crashLog.Record(new CrashReport(
                now, exitCode, now - _launchedAt, _crashes.Count, RecoveryAction.GiveUp,
                methodInFlight?.Invoke(), session.SessionPhase));
            SetStatusLocked(exitCode == StoreNewer ? EngineStatus.StoreNewer : EngineStatus.StoreTooOld, changes);
            return;
        }

        // The engine holding the pipe could not be adopted yet. It is finishing work it cannot
        // cancel, such as a first NPU compile, and accepts again once done. Waiting is not a crash
        if (exitCode == AlreadyServing)
        {
            // A gap well past one retry starts a fresh wait
            if (_waitingSince is null || now - _lastWait > ServerWait + TimeSpan.FromSeconds(30))
            {
                _waitingSince = now;
            }

            _lastWait = now;
            if (now - _waitingSince.Value < ServerWaitLimit)
            {
                SetStatusLocked(EngineStatus.Restarting, changes);
                CancelRelaunchLocked();
                _relaunch = clock.CreateTimer(_ => Relaunch(), null, ServerWait, Timeout.InfiniteTimeSpan);
                return;
            }
        }

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
