namespace ClinicAVT.App.Tests.TestDoubles;

/// <summary>A clock the test moves. Timers fire when it passes their due time.</summary>
internal sealed class FakeTimeProvider : TimeProvider
{
    private readonly List<FakeTimer> _timers = [];

    public DateTimeOffset Now { get; set; } = DateTimeOffset.UnixEpoch;

    public override DateTimeOffset GetUtcNow() => Now;

    /// <summary>The zone local time is read in, the machine's own when unset.</summary>
    public TimeZoneInfo? Zone { get; set; }

    public override TimeZoneInfo LocalTimeZone => Zone ?? base.LocalTimeZone;

    public override ITimer CreateTimer(
        TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period)
    {
        var timer = new FakeTimer(this, callback, state);
        timer.Change(dueTime, period);
        _timers.Add(timer);
        return timer;
    }

    /// <summary>Moves the clock, firing every timer that comes due on the way.</summary>
    public void Advance(TimeSpan by)
    {
        Now += by;
        foreach (var timer in _timers.ToList())
        {
            if (timer.Due is { } due && due <= Now)
            {
                timer.Due = null;
                timer.Fire();
            }
        }
    }

    private sealed class FakeTimer(FakeTimeProvider clock, TimerCallback callback, object? state) : ITimer
    {
        public DateTimeOffset? Due { get; set; }

        public bool Change(TimeSpan dueTime, TimeSpan period)
        {
            Due = dueTime == Timeout.InfiniteTimeSpan ? null : clock.Now + dueTime;
            return true;
        }

        public void Fire() => callback(state);

        public void Dispose() => clock._timers.Remove(this);

        public ValueTask DisposeAsync()
        {
            Dispose();
            return ValueTask.CompletedTask;
        }
    }
}
