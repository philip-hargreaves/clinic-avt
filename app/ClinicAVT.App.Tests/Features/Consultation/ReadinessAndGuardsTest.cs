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
        var shell = TestSession.Create(engine: engine, log: log);
        var (session, _, note) = shell;

        Assert.Contains("stuck in the graphics driver", shell.Line.LatestActivity);
        Assert.Contains(log.Lines, line => line.Contains("stray note host"));
        Assert.Empty(shell.Patient.Languages);
        Assert.Contains(log.Lines, line => line.Contains("translate/languages failed"));
    }

    [Fact]
    public async Task AMissingModelIsNamedOnConnectAndWhenRecordingIsRefused()
    {
        var engine = new FakeEngineClient(autoNotify: false) { MissingModels = { "asr", "diarisation", "segmentation" } };
        var log = new ListLogger();
        var shell = TestSession.Create(engine: engine, log: log);
        var (session, _, _) = shell;

        Assert.Equal(
            "Recording unavailable: the speech recognition and speaker recognition models are not installed",
            shell.Line.LatestActivity);

        engine.FailNext = method => method == "session/start"
            ? new EngineErrorException(Protocol.SessionErrorCode, "Session error",
                JsonSerializer.SerializeToElement("the speech recognition model is not installed"))
            : null;
        await session.StartRecordingAsync();

        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal("Recording could not start: the speech recognition model is not installed",
            shell.Line.LatestActivity);
        Assert.Contains(log.Lines, line => line.Contains("session/start failed"));
    }

    [Fact]
    public async Task AMicrophoneThatCannotOpenSaysWhyWhenRecordingIsRefused()
    {
        var engine = new FakeEngineClient(autoNotify: false);
        var shell = TestSession.Create(engine: engine);
        var (session, _, _) = shell;

        engine.FailNext = method => method == "session/start"
            ? new EngineErrorException(Protocol.CaptureFailedCode, "Capture failed",
                JsonSerializer.SerializeToElement("microphone access denied in Windows privacy settings"))
            : null;
        await session.StartRecordingAsync();

        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal(
            "Recording could not start: microphone access is off in Windows Settings, under Privacy & security, Microphone",
            shell.Line.LatestActivity);

        engine.FailNext = method => method == "session/start"
            ? new EngineErrorException(Protocol.CaptureFailedCode, "Capture failed",
                JsonSerializer.SerializeToElement("IAudioClient::Initialize failed, hr=0x8889000A"))
            : null;
        await session.StartRecordingAsync();

        Assert.Equal("Recording could not start: the microphone could not be opened", shell.Line.LatestActivity);
    }
}
