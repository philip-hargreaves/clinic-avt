using System.Text.Json;
using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Features.Settings;

public class NoteModelSettingsTest
{
    internal static FakeEngineClient TieredEngine()
    {
        var engine = new FakeEngineClient();
        engine.ExtraNoteModels.Add(("qwen3.6-35b-a3b-int4", "Qwen3.6 35B", "accuracy"));
        engine.ExtraNoteModels.Add(("qwen3.5-4b-int4", "Qwen3.5 4B", "constrained"));
        return engine;
    }

    private static AppPreferences Preferences() => new(new MemoryPreferencesStore());

    private static JsonElement NoteModel(
        string state, string tier, string name, string? detail = null) =>
        Params(
            new { state, tier, name, id = tier, seconds = 12.0, firstUse = false, detail });

    // A saved model other than the default loads on connect, before the page exists. The
    // picker still waits for it, since the engine refuses a switch mid-load
    [Fact]
    public void APageOpenedMidLoadGreysThePickerUntilTheLoadEnds()
    {
        var preferences = Preferences();
        preferences.NoteTier = "accuracy";
        var shell = TestSession.Settings(TieredEngine(), preferences, new FakeSession());
        shell.Models.ApplyNoteModel(ModelState.Loading, firstUse: false);

        var settings = shell.Get<NoteModelSettings>();

        Assert.True(settings.NoteModelEnabled);
        Assert.False(settings.PickerEnabled);
        shell.Models.ApplyNoteModel(ModelState.Ready, firstUse: false);
        Assert.True(settings.PickerEnabled);
    }

    [Fact]
    public void TheSavedTierSelectsQuietlyAndChoosingAnotherConfiguresTheEngineAndGreysUntilReady()
    {
        var preferences = Preferences();
        preferences.NoteTier = "accuracy";
        var engine = TieredEngine();
        var shell = TestSession.Settings(engine, preferences, new FakeSession());
        var settings = shell.Get<NoteModelSettings>();
        var status = shell.Line;

        Assert.Equal(["Qwen3.5 4B", "Qwen3.5 9B", "Qwen3.6 35B"], settings.NoteModelOptions);
        Assert.Equal(2, settings.NoteModelIndex);
        Assert.True(settings.NoteModelEnabled);
        // Restoring a saved tier sends no request
        Assert.DoesNotContain(engine.Requests, r => r.Method == "note/tier");

        settings.NoteModelIndex = 0;

        Assert.Equal("constrained", preferences.NoteTier);
        var request = engine.Requests.Single(r => r.Method == "note/tier");
        Assert.Contains("constrained", request.Params);
        Assert.False(settings.NoteModelEnabled, "greyed while the lane loads");
        Assert.True(settings.ModelLoading);
        Assert.False(settings.PickerEnabled);
        Assert.Equal(
            "Loading the note model · 0:00 · this can take a few minutes",
            settings.NoteModelCaption);
        Assert.Equal("Switching to Qwen3.5 4B · 0:00", status.LatestActivity);
        Assert.True(shell.Status.Busy);

        engine.RaiseNotification("note/model", NoteModel("loading", "constrained", "Qwen3.5 4B"));
        Assert.False(settings.NoteModelEnabled);

        engine.RaiseNotification("note/model", NoteModel("ready", "constrained", "Qwen3.5 4B"));
        Assert.True(settings.NoteModelEnabled);
        Assert.Equal("", settings.NoteModelStatus);
        Assert.StartsWith("Larger models", settings.NoteModelCaption);
        Assert.Equal(0, settings.NoteModelIndex);
        Assert.Equal("Ready", status.LatestActivity);
        Assert.False(shell.Status.Busy);
    }

    // A reconnect re-reads the store. Only a changed store rebuilds the bound
    // collection under the control
    [Fact]
    public void AReconnectWithTheSameModelsLeavesTheCollectionAlone()
    {
        var engine = TieredEngine();
        var settings = TestSession.Settings(engine, session: new FakeSession()).Get<NoteModelSettings>();
        var changes = 0;
        settings.NoteModelOptions.CollectionChanged += (_, _) => changes++;
        settings.NoteModelIndex = 2;

        engine.SetConnected(false);
        engine.SetConnected(true);

        Assert.Equal(0, changes);
        Assert.Equal(2, settings.NoteModelIndex);

        engine.ExtraNoteModels.RemoveAt(1);  // the 4B was uninstalled
        engine.SetConnected(false);
        engine.SetConnected(true);

        Assert.True(changes > 0);
        Assert.Equal(["Qwen3.5 9B", "Qwen3.6 35B"], settings.NoteModelOptions);
        Assert.Equal(1, settings.NoteModelIndex);
    }

    [Fact]
    public void ASingleStagedModelLeavesNothingToChooseAndAnUninstalledPreferenceFallsBack()
    {
        var settings = TestSession.Settings(new FakeEngineClient()).Get<NoteModelSettings>();

        Assert.Equal(["Qwen3.5 9B"], settings.NoteModelOptions);
        Assert.Equal(0, settings.NoteModelIndex);
        Assert.False(settings.NoteModelEnabled);
        Assert.Equal("Only one model installed", settings.NoteModelStatus);

        var preferences = Preferences();
        preferences.NoteTier = "accuracy";
        var uninstalled = TestSession.Settings(new FakeEngineClient(), preferences).Get<NoteModelSettings>();

        Assert.Equal("auto", preferences.NoteTier);
        Assert.Equal(0, uninstalled.NoteModelIndex);
        Assert.Contains("not installed", uninstalled.NoteModelStatus);
    }

    // A refused switch, as while a note is being written, keeps the resident model. Nothing is
    // resent and the optimistic loading state ends
    [Fact]
    public void AFailedLoadOrARefusedSwitchRevertsToTheTierThatWorked()
    {
        var preferences = Preferences();
        var engine = TieredEngine();
        var settings = TestSession.Settings(engine, preferences, new FakeSession()).Get<NoteModelSettings>();
        settings.NoteModelIndex = 2;
        engine.Requests.Clear();

        engine.RaiseNotification(
            "note/model", NoteModel("failed", "accuracy", "Qwen3.6 35B", "out of memory"));

        Assert.Equal("auto", preferences.NoteTier);
        Assert.Equal(1, settings.NoteModelIndex);
        Assert.Contains("out of memory", settings.NoteModelStatus);
        var back = engine.Requests.Single(r => r.Method == "note/tier");
        Assert.Contains("auto", back.Params);
        engine.RaiseNotification("note/model", NoteModel("ready", "default", "Qwen3.5 9B"));

        engine.Requests.Clear();
        engine.Failing.Add("note/tier");
        settings.NoteModelIndex = 0;

        Assert.Single(engine.Requests, r => r.Method == "note/tier");
        Assert.False(settings.ModelLoading);
        Assert.True(settings.PickerEnabled);
        Assert.Equal(1, settings.NoteModelIndex);
        Assert.Equal("auto", preferences.NoteTier);
        Assert.StartsWith("Could not switch", settings.NoteModelStatus);
    }

    // Until the user chooses, the engine's pick shows unsaved, so a changed machine is picked for
    // again. A saved choice still defers to the engine on what is resident
    [Fact]
    public void TheEnginesOwnPickIsShownUnsavedAndOnceChosenTheEngineStillSaysWhatIsResident()
    {
        var preferences = Preferences();
        var engine = TieredEngine();
        engine.NoteTier = "constrained";
        var settings = TestSession.Settings(engine, preferences, new FakeSession()).Get<NoteModelSettings>();

        Assert.Equal(0, settings.NoteModelIndex);
        engine.RaiseNotification("note/model", NoteModel("ready", "default", "Qwen3.5 9B"));
        Assert.Equal(1, settings.NoteModelIndex);
        Assert.Equal("auto", preferences.NoteTier);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "note/tier");

        settings.NoteModelIndex = 2;
        Assert.Equal("accuracy", preferences.NoteTier);
        engine.RaiseNotification("note/model", NoteModel("ready", "accuracy", "Qwen3.6 35B"));

        // The control follows a tier set by another shell instance
        engine.RaiseNotification("note/model", NoteModel("ready", "constrained", "Qwen3.5 4B"));
        Assert.Equal(0, settings.NoteModelIndex);
        Assert.Equal("constrained", preferences.NoteTier);
    }
}
