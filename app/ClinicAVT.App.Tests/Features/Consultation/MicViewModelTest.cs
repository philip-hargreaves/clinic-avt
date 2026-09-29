using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Features.Consultation;

public class MicViewModelTest
{
    private static AppPreferences TempPreferences() => new(new MemoryPreferencesStore());

    private static MicDevice Array(bool isDefault = true) =>
        new("{aa}", "Microphone Array (Cirrus Logic)", "Microphone Array", isDefault, false);

    private static MicDevice Jabra() =>
        new("{bb}", "Headset (Jabra Evolve2 65)", "Headset", false, true);

    [Fact]
    public async Task TheLabelSaysWhenThereIsNoMicrophoneAndNamesOneDeviceAsShortAsThatAllows()
    {
        var engine = new FakeEngineClient();
        var mic = new TestShell(engine).Get<MicViewModel>();

        await mic.RefreshAsync();
        Assert.False(mic.HasDevices);
        Assert.Empty(mic.Rows);
        Assert.Equal("No microphone found", mic.Label);
        Assert.Equal("No microphone found - connect one to record", mic.NoDevicesText);
        Assert.Equal("", mic.MicId);

        // Single built-in microphone, the common case
        engine.AudioInputs = [Array()];
        await mic.RefreshAsync();
        Assert.Equal("Microphone Array", mic.Label);

        // Distinct endpoint names still label by endpoint. The rows name the default,
        // check the current choice and caption the Bluetooth device
        engine.AudioInputs = [Array(), Jabra()];
        await mic.RefreshCommand.ExecuteAsync(null);
        Assert.Equal("Microphone Array", mic.Label);
        Assert.Equal(
            [("Microphone Array (Cirrus Logic)  (default)", "", true), ("Headset (Jabra Evolve2 65)", "    Bluetooth call mode - reduced recording quality", false)],
            mic.Rows.Select(r => (r.Label, r.Note, r.IsChecked)));

        // When three devices share the endpoint name the full name tells them apart
        engine.AudioInputs =
        [
            new("{aa}", "Microphone (USB Audio)", "Microphone", true, false),
            new("{bb}", "Microphone (Jabra)", "Microphone", false, true),
            new("{cc}", "Microphone (Realtek)", "Microphone", false, false),
        ];
        await mic.RefreshAsync();
        Assert.Equal("Microphone (USB Audio)", mic.Label);
    }

    [Fact]
    public async Task TheChoicePersistsAGoneChoiceFallsToTheDefaultAndStartSendsTheSavedOne()
    {
        var preferences = TempPreferences();
        var engine = new FakeEngineClient();
        var mic = new TestShell(engine, preferences).Get<MicViewModel>();
        engine.AudioInputs = [Array(), Jabra()];
        await mic.RefreshAsync();

        mic.SelectCommand.Execute("{bb}");
        Assert.Equal("{bb}", mic.MicId);
        Assert.Equal("{bb}", preferences.MicId);
        Assert.Equal([false, true], mic.Rows.Select(r => r.IsChecked));

        // An unplugged headset falls back to the default and keeps the saved choice
        engine.AudioInputs = [Array()];
        await mic.RefreshAsync();
        Assert.Equal("{aa}", mic.MicId);
        Assert.Equal("{bb}", preferences.MicId);

        engine.AudioInputs = [Array(), Jabra()];
        await mic.RefreshAsync();
        Assert.Equal("{bb}", mic.MicId);

        var shell = TestSession.Create(preferences);
        var (session, sessionEngine, _) = shell;
        await session.StartRecordingAsync();
        var start = sessionEngine.Requests.Single(r => r.Method == "session/start");
        Assert.Contains("{bb}", start.Params);
    }
}
