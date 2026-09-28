using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Features.Consultation;

public class SessionStateMachineTest
{
    [Fact]
    public async Task AFailedStartStaysIdleAndATimedOutStopRecoversToIdle()
    {
        var engine = new FakeEngineClient(autoNotify: false)
        {
            FailNext = method => method == "session/start"
                ? new InvalidOperationException("the speech model is still loading")
                : null,
        };
        var log = new ListLogger();
        var (session, _, _) = TestSession.Create(engine: engine, log: log);

        await session.StartRecordingAsync();
        Assert.Equal(SessionState.Idle, session.State);
        Assert.Contains(log.Lines, line => line.Contains("still loading"));

        await session.StartRecordingAsync();
        engine.FailNext = method => method == "session/stop" ? new TaskCanceledException() : null;
        await session.StopRecordingAsync();

        Assert.Equal("Stop failed, consultation kept: the engine did not answer in time",
            session.Status.LatestActivity);
        Assert.Equal(SessionState.Idle, session.State);  // never wedged in Finalising
    }

    [Fact]
    public async Task FullLifecycleAdvancesThroughEveryStateAndCancelReturnsToIdle()
    {
        var (session, engine, _) = TestSession.Create();
        Assert.Equal(SessionState.Idle, session.State);

        await session.StartRecordingAsync();
        Assert.Equal(SessionState.Recording, session.State);

        await session.StopRecordingAsync();
        Assert.Equal(SessionState.Finalising, session.State);

        engine.RaiseNotification("note/ready");
        Assert.Equal(SessionState.Review, session.State);

        session.FinishConsultation();
        Assert.Equal(SessionState.Idle, session.State);

        await session.StartRecordingAsync();
        await session.CancelRecordingAsync();
        Assert.Equal(SessionState.Idle, session.State);
    }

    [Fact]
    public async Task IllegalTransitionsAndUnknownNotificationsAreIgnored()
    {
        var (session, engine, _) = TestSession.Create();

        // Nothing but start is legal from idle
        await session.StopRecordingAsync();
        await session.CancelRecordingAsync();
        session.FinishConsultation();
        Assert.Equal(SessionState.Idle, session.State);

        // A note/ready that arrives outside finalising must not move the state
        engine.RaiseNotification("note/ready");
        Assert.Equal(SessionState.Idle, session.State);

        await session.StartRecordingAsync();
        await session.StartRecordingAsync();
        Assert.Equal(SessionState.Recording, session.State);

        session.FinishConsultation();
        Assert.Equal(SessionState.Recording, session.State);

        engine.RaiseNotification("engine/unheard-of");
        Assert.Equal(SessionState.Recording, session.State);
    }
}
