using System.Text.Json;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Demo;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Features.Consultation;

/// <summary>A stored consultation played back as a demo walks the real states.</summary>
public class PlaybackSessionTest
{
    private static readonly DemoMaster Elbow = new("s-real", 542);

    private static string MastersFile(params (string Name, string Id, double Seconds)[] masters)
    {
        var path = Path.Combine(Path.GetTempPath(), $"clinicavt-masters-{Guid.NewGuid():N}.json");
        File.WriteAllText(path, "{" + string.Join(",", masters.Select(m =>
            $"\"{m.Name}\":{{\"id\":\"{m.Id}\",\"audioSeconds\":{m.Seconds}}}")) + "}");
        return path;
    }

    private static (ConsultationViewModel Session, FakeEngineClient Engine, DemoMode Demo) DemoSession(
        params (string Name, string Id, double Seconds)[] masters)
    {
        var demo = new DemoMode(null, MastersFile(masters), []);
        var (session, engine, _) = TestSession.Create(demo: demo);
        return (session, engine, demo);
    }

    [Fact]
    public async Task PlaybackWalksTheSameStatesAsARecordingAndOnlyASeededSampleIsMarkedAsADemo()
    {
        var (session, engine, note) = TestSession.Create();

        await session.StartPlaybackAsync(Elbow);
        Assert.Equal(SessionState.Recording, session.State);
        Assert.Equal(Elbow, session.Recorder.ActivePlayback);
        Assert.Null(session.ActiveReplay);
        Assert.True(session.Status.Demo);
        Assert.True(session.Status.MicVisible);

        engine.RaiseNotification("audio.level", Params(new { level = 0.4, clipped = false, seconds = 300.5 }));
        Assert.Equal(300.5, session.AudioSeconds);

        // The engine reports the finalise stages while stop blocks
        var staged = FinalisePhase.None;
        engine.BeforeReply = method =>
        {
            if (method == "session/stop")
            {
                engine.RaiseNotification("session/progress", Params(new { stage = "speakers" }));
                staged = session.Phase;
            }
        };
        await session.StopRecordingAsync();
        Assert.Equal(FinalisePhase.Speakers, staged);
        Assert.Equal(SessionState.Finalising, session.State);
        Assert.Null(session.Recorder.ActivePlayback);
        Assert.Equal(FinalisePhase.Note, session.Phase);

        engine.RaiseNotification("note/partial", Params(new { text = "Presented with" }));
        engine.RaiseNotification("note/ready", Params(new { text = "Presented with a swollen elbow." }));
        Assert.Equal(SessionState.Review, session.State);
        engine.RaiseNotification("patient/ready", Params(new { text = "You came in about your elbow." }));
        Assert.Equal("Presented with a swollen elbow.", note.ClinicalNoteText);
        Assert.Equal("You came in about your elbow.", note.PatientInfoText);
        Assert.True(session.Status.Demo, "the marker stays through the review");

        await session.CloseReviewAsync();
        Assert.Equal(SessionState.Idle, session.State);
        Assert.False(session.Status.Demo);

        // A stored review carries the marker only for a seeded sample
        Assert.True(await session.OpenStoredSessionAsync("s-sample", demo: true));
        Assert.True(session.Status.Demo);
        await session.CloseReviewAsync();
        Assert.False(session.Status.Demo);
        Assert.True(await session.OpenStoredSessionAsync("s-real"));
        Assert.False(session.Status.Demo);
    }

    [Fact]
    public async Task ANoteThatStreamsDuringTheTranscriptFetchKeepsItsPanes()
    {
        var (session, engine, _) = TestSession.Create();
        await session.StartPlaybackAsync(Elbow);
        // The first token lands while the sealed transcript is still being fetched
        engine.BeforeReply = method =>
        {
            if (method == "session/transcript")
            {
                engine.RaiseNotification("note/partial", Params(new { text = "Presented" }));
            }
        };

        await session.StopRecordingAsync();

        Assert.Equal(FinalisePhase.Streaming, session.Phase);
    }

    [Fact]
    public async Task CancelClearsThePlaybackAndItsClockStopsItAtTheEnd()
    {
        var (session, engine, _) = TestSession.Create();
        await session.StartPlaybackAsync(Elbow);

        await session.CancelRecordingAsync();
        Assert.Equal(SessionState.Idle, session.State);
        Assert.Null(session.Recorder.ActivePlayback);
        Assert.False(session.Status.Demo);

        await session.StartPlaybackAsync(Elbow);
        engine.RaiseNotification("audio.level", Params(new { level = 0.4, clipped = false, seconds = 271.0 }));
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/stop");

        engine.RaiseNotification("audio.level", Params(new { level = 0.4, clipped = false, seconds = 542.0 }));
        Assert.Contains(engine.Requests, r => r.Method == "session/stop");
        Assert.Equal(SessionState.Finalising, session.State);
    }

    [Fact]
    public async Task InDemoModeRecordPlaysTheChosenSavedRunBack()
    {
        var (session, engine, demo) = DemoSession(("Elbow swelling", "s-elbow", 540), ("Chest pain", "s-chest", 457));
        demo.Enabled = true;
        demo.Track = "Chest pain";
        Assert.True(session.Status.Demo, "the badge shows as soon as the mode is on");

        await session.StartRecordingAsync();

        var start = engine.Requests.Single(r => r.Method == "session/start");
        using var json = JsonDocument.Parse(start.Params);
        Assert.Equal("s-chest", json.RootElement.GetProperty("playback").GetProperty("id").GetString());
        Assert.False(json.RootElement.TryGetProperty("micId", out _));
        Assert.Equal(new DemoMaster("s-chest", 457), session.Recorder.ActivePlayback);
        Assert.Equal(SessionState.Recording, session.State);
    }

    [Fact]
    public async Task WithDemoModeOffRecordListensToTheMicrophoneAndCountsTenthsOfASecond()
    {
        var (session, engine, demo) = DemoSession(("Elbow swelling", "s-elbow", 540));
        Assert.False(session.Status.Demo);

        await session.StartRecordingAsync();

        var start = engine.Requests.Single(r => r.Method == "session/start");
        Assert.Contains("micId", start.Params);
        Assert.Null(session.Recorder.ActivePlayback);

        engine.RaiseNotification("audio.level", Params(new { level = 0.4, clipped = false }));
        engine.RaiseNotification("audio.level", Params(new { level = 0.4, clipped = false }));
        Assert.Equal(0.2, session.AudioSeconds, 6);

        demo.Enabled = true;
        Assert.True(session.Status.Demo, "switching on mid-session lights the badge");
        demo.Enabled = false;
        Assert.False(session.Status.Demo);
    }

    [Fact]
    public async Task DemoModeWithoutASavedRunListensToTheMicrophone()
    {
        var (session, engine, demo) = DemoSession();
        demo.Enabled = true;

        await session.StartRecordingAsync();

        Assert.Contains("micId", engine.Requests.Single(r => r.Method == "session/start").Params);
    }

    [Fact]
    public void DemoModePersistsAndFallsBackToTheFirstSavedRun()
    {
        var path = Path.Combine(Path.GetTempPath(), $"clinicavt-prefs-{Guid.NewGuid():N}.json");
        var masters = MastersFile(("Elbow swelling", "s-elbow", 540), ("Chest pain", "s-chest", 457));
        var demo = new DemoMode(new AppPreferences(path), masters, []);
        Assert.Equal(new DemoMaster("s-elbow", 540), demo.Master);
        Assert.Equal(["Elbow swelling", "Chest pain"], demo.Tracks);

        demo.Enabled = true;
        demo.Track = "Chest pain";

        var reloaded = new DemoMode(AppPreferences.Load(path), masters, []);
        Assert.True(reloaded.Enabled);
        Assert.Equal("s-chest", reloaded.Master!.SessionId);
        Assert.Null(new DemoMode(null, Path.Combine(Path.GetTempPath(), "missing.json"), []).Master);

        var broken = Path.Combine(Path.GetTempPath(), $"clinicavt-masters-{Guid.NewGuid():N}.json");
        File.WriteAllText(broken, """{"Elbow swelling": {"id": "", "audioSeconds": 0}, "Chest pain": "not an object"}""");
        Assert.Null(new DemoMode(null, broken, []).Master);

        // A run recorded while the app is open shows once the switch turns on
        var late = Path.Combine(Path.GetTempPath(), $"clinicavt-masters-{Guid.NewGuid():N}.json");
        var waiting = new DemoMode(null, late, []);
        Assert.Empty(waiting.Tracks);
        File.WriteAllText(late, """{"Gout": {"id": "s-gout", "audioSeconds": 300}}""");
        waiting.Enabled = true;
        Assert.Equal(["Gout"], waiting.Tracks);
    }
}
