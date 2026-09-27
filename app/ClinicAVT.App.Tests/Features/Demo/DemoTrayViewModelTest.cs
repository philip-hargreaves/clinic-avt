using System.Text.Json;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Demo;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;

namespace ClinicAVT.App.Tests.Features.Demo;

public class DemoTrayViewModelTest
{
    private static DemoTrack Track(string name = "Elbow swelling") =>
        new(name, $"C:/demo/{name}.wav");

    [Fact]
    public async Task PlayWaitsForTheEngineSendsTheReplayAtTheChosenSpeedPausesMonitorsAndStopFinalises()
    {
        var (session, engine, _) = TestSession.Create();
        engine.SetConnected(false);
        var preferences = new AppPreferences(new MemoryPreferencesStore());
        var tray = new DemoTrayViewModel(session, new FakeFilePicker(), [Track()], preferences) { MonitorAudio = true };
        Assert.False(tray.Visible);
        preferences.DemoTrayEnabled = true;
        preferences.Save();
        Assert.True(tray.Visible);

        Assert.False(tray.PlayCommand.CanExecute(null));
        engine.SetConnected(true);
        Assert.True(tray.PlayCommand.CanExecute(null));

        Assert.Equal(1, tray.Speed);
        tray.CycleSpeedCommand.Execute(null);
        Assert.Equal(4, tray.Speed);

        await tray.PlayCommand.ExecuteAsync(null);

        var start = engine.Requests.Single(r => r.Method == "session/start");
        using var json = JsonDocument.Parse(start.Params);
        var replay = json.RootElement.GetProperty("replay");
        Assert.Equal("C:/demo/Elbow swelling.wav", replay.GetProperty("path").GetString());
        Assert.Equal(4, replay.GetProperty("speed").GetDouble());
        Assert.True(replay.GetProperty("monitor").GetBoolean());
        Assert.Equal(SessionState.Recording, session.State);
        Assert.False(tray.PlayCommand.CanExecute(null));
        Assert.True(tray.StopCommand.CanExecute(null));

        await tray.TogglePauseCommand.ExecuteAsync(null);
        Assert.True(session.Paused);
        await tray.TogglePauseCommand.ExecuteAsync(null);
        Assert.False(session.Paused);
        Assert.Equal(2, engine.Requests.Count(r => r.Method == "session/pause"));

        tray.MonitorAudio = false;
        tray.MonitorAudio = true;
        Assert.Equal(2, engine.Requests.Count(r => r.Method == "session/monitor"));

        await tray.StopCommand.ExecuteAsync(null);
        Assert.Equal(SessionState.Finalising, session.State);

        // The speed cycles round to 1x
        tray.CycleSpeedCommand.Execute(null);
        tray.CycleSpeedCommand.Execute(null);
        tray.CycleSpeedCommand.Execute(null);
        Assert.Equal(1, tray.Speed);
    }

    [Fact]
    public async Task ProgressFollowsDeliveredAudioAndTheReplayStopsItselfAtTheEndOfTheTrack()
    {
        var (session, engine, _) = TestSession.Create();
        var wav = SilenceWav.Write(seconds: 2);
        var tray = new DemoTrayViewModel(session, new FakeFilePicker(), [new DemoTrack("Silence", wav)]);
        await tray.PlayCommand.ExecuteAsync(null);

        for (var i = 0; i < 10; i++)
        {
            engine.RaiseNotification("audio.level", JsonSerializer.SerializeToElement(
                new { level = 0.5, clipped = false }));
        }

        Assert.Equal(0.5, tray.ProgressFraction, 3);
        Assert.Equal("0:01 / 0:02", tray.ProgressText);
        Assert.Equal(SessionState.Recording, session.State);

        for (var i = 0; i < 10; i++)
        {
            engine.RaiseNotification("audio.level", JsonSerializer.SerializeToElement(
                new { level = 0.5, clipped = false }));
        }

        Assert.Equal(SessionState.Finalising, session.State);
    }

    [Fact]
    public void BrowseAddsASelectableTrack()
    {
        var (session, _, _) = TestSession.Create();
        var tray = new DemoTrayViewModel(session, new FakeFilePicker(), [Track()]);

        tray.UseTrack("C:/elsewhere/my_recording.wav");

        Assert.Equal(2, tray.Tracks.Count);
        Assert.Equal("my_recording", tray.SelectedTrack!.Name);
    }
}
