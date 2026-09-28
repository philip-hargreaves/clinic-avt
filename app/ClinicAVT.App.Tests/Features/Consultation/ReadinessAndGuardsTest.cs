using System.Text.Json;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;

namespace ClinicAVT.App.Tests.Features.Consultation;

public class ReadinessAndGuardsTest
{
    [Fact]
    public void AStrayNoteHostAndAFailedLanguageListAreBothReportedAtStart()
    {
        var engine = new FakeEngineClient(autoNotify: false)
        {
            StrayNoteHost = true,
            FailNext = method => method == "translate/languages"
                ? new InvalidOperationException("no translator installed")
                : null,
        };

        var log = new ListLogger();
        var (session, _, note) = TestSession.Create(engine: engine, log: log);

        Assert.Contains("stuck in the graphics driver", session.Status.LatestActivity);
        Assert.Contains(log.Lines, line => line.Contains("stray note host"));
        Assert.Empty(note.Languages);
        Assert.Contains(log.Lines, line => line.Contains("translate/languages failed"));
    }

    // An engine without a model says so on connect, and the start it refuses says why
    [Fact]
    public async Task AMissingModelIsNamedOnConnectAndWhenRecordingIsRefused()
    {
        var engine = new FakeEngineClient(autoNotify: false) { MissingModels = { "asr", "diarisation", "segmentation" } };
        var log = new ListLogger();
        var (session, _, _) = TestSession.Create(engine: engine, log: log);

        Assert.Equal(
            "Recording unavailable: the speech recognition and speaker recognition models are not installed",
            session.Status.LatestActivity);

        engine.FailNext = method => method == "session/start"
            ? new EngineErrorException(Protocol.SessionErrorCode, "Session error",
                JsonSerializer.SerializeToElement("the speech recognition model is not installed"))
            : null;
        await session.StartRecordingAsync();

        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal("Recording could not start: the speech recognition model is not installed",
            session.Status.LatestActivity);
        Assert.Contains(log.Lines, line => line.Contains("session/start failed"));
    }
}
