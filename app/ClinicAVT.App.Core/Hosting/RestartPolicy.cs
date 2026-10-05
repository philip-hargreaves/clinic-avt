namespace ClinicAVT.App.Core.Hosting;

public enum RecoveryAction
{
    Restart,
    GiveUp,
}

/// <summary>
/// Relaunches the engine with a growing backoff until a crash-storm cutoff. A consultation in
/// progress resumes on the new engine.
/// </summary>
public static class RestartPolicy
{
    // Stops a crash loop: five crashes within the window give up
    public const int StormLimit = 5;

    public static readonly TimeSpan StormWindow = TimeSpan.FromMinutes(3);

    public static readonly TimeSpan MaxBackoff = TimeSpan.FromSeconds(8);

    public static RecoveryAction Decide(IReadOnlyList<DateTimeOffset> crashes, DateTimeOffset now)
    {
        var recent = crashes.Count(crash => now - crash <= StormWindow);
        return recent >= StormLimit ? RecoveryAction.GiveUp : RecoveryAction.Restart;
    }

    /// <summary>
    /// No wait after the first crash, then doubling from one second, so a passing fault has time to
    /// clear before the limit is reached.
    /// </summary>
    public static TimeSpan Backoff(int recentCrashes)
    {
        if (recentCrashes <= 1)
        {
            return TimeSpan.Zero;
        }

        var seconds = Math.Min(MaxBackoff.TotalSeconds, Math.Pow(2, recentCrashes - 2));
        return TimeSpan.FromSeconds(seconds);
    }
}
