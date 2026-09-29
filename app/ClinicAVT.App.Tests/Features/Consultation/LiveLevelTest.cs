using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Tests.Support;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Features.Consultation;

public class LiveLevelTest
{
    [Fact]
    public async Task LevelsReachTheStatusBarAndAnInterruptionMidRecordingOrFinaliseTellsTheClinicianAndResets()
    {
        var shell = TestSession.Create();
        var (session, engine, note) = shell;

        // A stray interruption while idle is ignored
        engine.RaiseNotification(
            "session/interrupted", Params(new { reason = "failed", detail = "stray" }));
        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal("", shell.Line.LatestActivity);

        await session.StartRecordingAsync();
        engine.RaiseNotification("audio.level", Params(new { level = 0.8, clipped = true }));
        Assert.Equal(0.8, shell.Activity.Level);

        engine.RaiseNotification(
            "session/interrupted", Params(new { reason = "deviceLost", detail = "unplugged" }));

        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal(NotePipelineState.Pending, note.PipelineState);
        Assert.Contains("unplugged", shell.Line.LatestActivity);
        Assert.Equal(0, shell.Activity.Level);

        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification(
            "session/interrupted", Params(new { reason = "failed", detail = "driver gone" }));
        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal(NotePipelineState.Pending, note.PipelineState);
    }
}
