using System.Text.Json;
using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;
using static ClinicAVT.App.Tests.Support.Waits;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Features.Settings;

public class SettingsViewModelTest
{
    private static AppPreferences TempPreferences() =>
        new(Path.Combine(Path.GetTempPath(), Path.GetRandomFileName()));

    // Low-power mode moves speech recognition inside the running engine, so the note model
    // stays loaded and nothing restarts
    [Fact]
    public void LaunchRestoresQuietlyAndTheNpuToggleSavesAndMovesSpeechRecognitionInPlace()
    {
        var path = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        var preferences = new AppPreferences(path)
        {
            NpuTranscription = true,
            KeepConsultations = true,
            DemoTrayEnabled = true,
            Theme = "light",
        };
        var engine = new FakeEngineClient();
        var status = TestSession.Status(engine);
        var asked = 0;

        var dialogs = new FakeDialogService { Answer = false, OnConfirm = () => asked++ };
        var settings = new SettingsViewModel(preferences, new FakeSession(), status,
            client: new EngineApi(engine), dialogs: dialogs);

        Assert.True(settings.Appearance.NpuTranscription);
        Assert.True(settings.Privacy.KeepConsultations);
        Assert.True(settings.Appearance.DemoTrayEnabled);
        Assert.Equal("light", settings.Appearance.Theme);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "asr/device");  // a launch moves nothing
        Assert.Equal(0, asked);
        Assert.False(File.Exists(path), "launch restore must not re-save");

        settings.Appearance.NpuTranscription = false;

        Assert.False(preferences.NpuTranscription);
        Assert.Contains("\"GPU\"", engine.Requests.Single(r => r.Method == "asr/device").Params);
        engine.RaiseNotification("asr/device", Params(new { device = "GPU", state = "ready" }));
        Assert.Equal("Ready", status.LatestActivity);

        // A move that fails puts the toggle and the saved choice back
        settings.Appearance.NpuTranscription = true;
        engine.RaiseNotification("asr/device", Params(new { device = "NPU", state = "failed", detail = "no NPU" }));
        Assert.False(settings.Appearance.NpuTranscription);
        Assert.False(preferences.NpuTranscription);
        Assert.Contains("no NPU", status.LatestActivity);
    }

    [Fact]
    public void SeedDataFollowsTheSwitchPersistsAndALaunchWithItOnSeedsQuietly()
    {
        var preferences = TempPreferences();
        var engine = new FakeEngineClient();
        var status = TestSession.Status(engine);
        var settings = new SettingsViewModel(preferences, status: status, client: new EngineApi(engine));
        Assert.False(settings.Privacy.SeedDataEnabled);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "demo/seed");

        settings.Privacy.SeedDataEnabled = true;
        Assert.True(preferences.SeedDataEnabled);
        Assert.True(engine.SamplesSeeded);
        Assert.Contains("8 sample consultations added", status.LatestActivity);

        settings.Privacy.SeedDataEnabled = false;
        Assert.False(preferences.SeedDataEnabled);
        Assert.False(engine.SamplesSeeded);
        Assert.Contains("8 sample consultations removed", status.LatestActivity);

        // A relaunch with the switch on and the samples present seeds once and says nothing
        preferences.SeedDataEnabled = true;
        engine.SamplesSeeded = true;
        engine.Requests.Clear();
        var relaunched = TestSession.Status(engine);
        var again = new SettingsViewModel(preferences, status: relaunched, client: new EngineApi(engine));

        Assert.True(again.Privacy.SeedDataEnabled);
        Assert.Single(engine.Requests, r => r.Method == "demo/seed");
        Assert.DoesNotContain("sample", relaunched.LatestActivity);
    }

    [Fact]
    public async Task DeleteAllAsksFirstThenErasesConsultationsOnlyAndTurnsSeedDataOff()
    {
        var preferences = TempPreferences();
        var engine = new FakeEngineClient { StoredSessions = 3 };
        engine.GuidanceDocuments.Add(Document(1, "Gout", "ready", 41));
        var status = TestSession.Status(engine);
        var dialogs = new FakeDialogService { Answer = false };
        var settings = new SettingsViewModel(preferences, status: status, client: new EngineApi(engine), dialogs: dialogs);
        settings.Privacy.SeedDataEnabled = true;

        await settings.Privacy.DeleteAllConsultationsCommand.ExecuteAsync(null);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/deleteAll");
        Assert.True(settings.Privacy.SeedDataEnabled);

        dialogs.Answer = true;
        await settings.Privacy.DeleteAllConsultationsCommand.ExecuteAsync(null);
        Assert.Single(engine.Requests, r => r.Method == "session/deleteAll");
        Assert.Contains("11 consultations deleted", status.LatestActivity);
        Assert.False(settings.Privacy.SeedDataEnabled);
        Assert.False(preferences.SeedDataEnabled);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "demo/clear");  // nothing left to clear
        Assert.Equal(0, engine.StoredSessions);
        Assert.Single(settings.Guidance.Documents);  // guideline documents are not consultations
    }

    // A date cannot say what a backup holds, so the engine counts what is in no backup
    [Fact]
    public async Task DeleteAllSaysWhatNoBackupHoldsAndKeepsReflectionsUnlessTicked()
    {
        var preferences = TempPreferences();
        var engine = new FakeEngineClient { StoredSessions = 40 };
        var dialogs = new FakeDialogService();
        var settings = new SettingsViewModel(preferences, status: TestSession.Status(engine),
            client: new EngineApi(engine), dialogs: dialogs);

        await settings.Privacy.DeleteAllConsultationsCommand.ExecuteAsync(null);
        Assert.Equal("No consultations are backed up. This can't be undone.", dialogs.LastContent);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "archive/summary");
        Assert.Contains("\"deleteReflections\":false", engine.Requests.Single(r => r.Method == "session/deleteAll").Params);

        preferences.LastBackup = new LastBackup(
            "2026-07-01T00:00:00Z", "2026-09-01T00:00:00Z", "2026-09-01T08:30:00Z", 32);
        engine.Responses["archive/summary"] = new { consultations = 38, reflections = 12, unfinished = 0, uncovered = 6 };
        dialogs.Ticked = true;
        await settings.Privacy.DeleteAllConsultationsCommand.ExecuteAsync(null);

        Assert.Equal("6 consultations aren't backed up. This can't be undone.", dialogs.LastContent);
        var covered = JsonDocument.Parse(engine.Requests.Single(r => r.Method == "archive/summary").Params)
            .RootElement.GetProperty("covered");
        Assert.Equal("2026-09-01T08:30:00Z", covered.GetProperty("at").GetString());
        Assert.Contains("\"deleteReflections\":true", engine.Requests.Last(r => r.Method == "session/deleteAll").Params);
        Assert.Contains("Last backup: 1 Sep 2026, 32 consultations.", settings.Privacy.BackupDescription);
    }

    // Everything that restarts or reconfigures the engine waits for the consultation to end
    [Fact]
    public async Task DuringAConsultationDeleteAllTheNpuToggleAndTheTierAreRefused()
    {
        var preferences = TempPreferences();
        var engine = TieredEngine();
        engine.StoredSessions = 3;
        var status = TestSession.Status(engine);
        var settings = new SettingsViewModel(preferences, new FakeSession { ConsultationInProgress = true },
            status: status, client: new EngineApi(engine), dialogs: new FakeDialogService());

        await settings.Privacy.DeleteAllConsultationsCommand.ExecuteAsync(null);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/deleteAll");
        Assert.Contains("finish the consultation", status.LatestActivity);

        settings.Appearance.NpuTranscription = true;
        Assert.False(settings.Appearance.NpuTranscription);
        Assert.False(preferences.NpuTranscription);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "asr/device");

        settings.NoteModel.NoteModelIndex = 2;
        Assert.Equal(1, settings.NoteModel.NoteModelIndex);
        Assert.Equal("auto", preferences.NoteTier);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "note/tier");
    }

    // Deleting everything closes the review first, so the screen never shows erased data
    [Fact]
    public async Task AFinishedConsultationOnScreenBlocksNothingAndDeleteAllClosesItsReview()
    {
        var preferences = TempPreferences();
        var engine = TieredEngine();
        engine.StoredSessions = 3;
        var status = TestSession.Status(engine);
        var session = new FakeSession { ReviewedSessionId = "s1" };
        var dialogs = new FakeDialogService();
        var settings = new SettingsViewModel(preferences, session,
            status: status, client: new EngineApi(engine), dialogs: dialogs);

        await settings.Privacy.BackUpCommand.ExecuteAsync(null);
        Assert.Equal(1, dialogs.BackupsRun);

        settings.Appearance.NpuTranscription = true;
        Assert.Contains(engine.Requests, r => r.Method == "asr/device");

        settings.NoteModel.NoteModelIndex = 2;
        Assert.Contains(engine.Requests, r => r.Method == "note/tier");

        await settings.Privacy.DeleteAllConsultationsCommand.ExecuteAsync(null);
        Assert.Equal(1, session.ReviewsEnded);
        Assert.Contains(engine.Requests, r => r.Method == "session/deleteAll");
        Assert.DoesNotContain("finish the consultation", status.LatestActivity);
    }

    [Fact]
    public async Task KeepConsultationsDefaultsOffAndTurningOnIsConfirmedNeverJustToggled()
    {
        var preferences = TempPreferences();
        var asked = 0;
        var dialogs = new FakeDialogService { Answer = false, OnConfirm = () => asked++ };
        var settings = new SettingsViewModel(preferences, dialogs: dialogs);
        Assert.False(settings.Privacy.KeepConsultations, "save nothing unless the clinician opts in");

        settings.Privacy.KeepConsultations = true;
        await WaitUntilAsync(() => asked == 1);
        Assert.Equal(1, asked);
        Assert.False(settings.Privacy.KeepConsultations, "declined: the toggle stays off");
        Assert.False(preferences.KeepConsultations, "and nothing was persisted");

        dialogs.Answer = true;
        settings.Privacy.KeepConsultations = true;
        await WaitUntilAsync(() => asked == 2);
        Assert.Equal(2, asked);
        Assert.True(settings.Privacy.KeepConsultations);
        Assert.True(preferences.KeepConsultations);

        settings.Privacy.KeepConsultations = false;  // turning off needs no confirmation
        Assert.Equal(2, asked);
        Assert.False(preferences.KeepConsultations);
    }

    [Fact]
    public void TheDocumentsListIsClosedAndAFailureOpensItOnce()
    {
        var engine = new FakeEngineClient();
        engine.GuidanceDocuments.Add(Document(1, "Gout", "ready", 41));
        var settings = new SettingsViewModel(TempPreferences(), client: new EngineApi(engine));

        Assert.Equal("1 document", settings.Guidance.DocumentsSummary);
        Assert.False(settings.Guidance.DocumentsExpanded);

        engine.RaiseNotification("guidance/document", Params(Document(2, "Letter", "failed", error: "password")));
        Assert.True(settings.Guidance.DocumentsExpanded);

        settings.Guidance.DocumentsExpanded = false;
        engine.RaiseNotification("guidance/document", Params(Document(3, "PMR", "ready", 10)));
        Assert.False(settings.Guidance.DocumentsExpanded, "the same failure does not reopen it");
    }

    private static FakeEngineClient TieredEngine()
    {
        var engine = new FakeEngineClient();
        engine.ExtraNoteModels.Add(("qwen3.6-35b-a3b-int4", "Qwen3.6 35B", "accuracy"));
        engine.ExtraNoteModels.Add(("qwen3.5-4b-int4", "Qwen3.5 4B", "constrained"));
        return engine;
    }

    private static JsonElement NoteModel(
        string state, string tier, string name, string? detail = null) =>
        Params(
            new { state, tier, name, id = tier, seconds = 12.0, firstUse = false, detail });

    // A saved model other than the default loads on connect, before the page exists. The
    // picker still waits for it, since the engine refuses a switch mid-load
    [Fact]
    public void APageOpenedMidLoadGreysThePickerUntilTheLoadEnds()
    {
        var preferences = TempPreferences();
        preferences.NoteTier = "accuracy";
        var status = TestSession.Status();
        status.ApplyNoteModel("loading", firstUse: false);

        var settings = new SettingsViewModel(preferences, client: new EngineApi(TieredEngine()), session: new FakeSession(), status: status);

        Assert.True(settings.NoteModel.NoteModelEnabled);
        Assert.False(settings.NoteModel.PickerEnabled);
        status.ApplyNoteModel("ready", firstUse: false);
        Assert.True(settings.NoteModel.PickerEnabled);
    }

    [Fact]
    public void TheSavedTierSelectsQuietlyAndChoosingAnotherConfiguresTheEngineAndGreysUntilReady()
    {
        var preferences = TempPreferences();
        preferences.NoteTier = "accuracy";
        var engine = TieredEngine();
        var status = TestSession.Status(engine);
        var settings = new SettingsViewModel(preferences, client: new EngineApi(engine), session: new FakeSession(), status: status);

        Assert.Equal(["Qwen3.5 4B", "Qwen3.5 9B", "Qwen3.6 35B"], settings.NoteModel.NoteModelOptions);
        Assert.Equal(2, settings.NoteModel.NoteModelIndex);
        Assert.True(settings.NoteModel.NoteModelEnabled);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "note/tier");  // restoring a saved tier sends no request

        settings.NoteModel.NoteModelIndex = 0;

        Assert.Equal("constrained", preferences.NoteTier);
        var request = engine.Requests.Single(r => r.Method == "note/tier");
        Assert.Contains("constrained", request.Params);
        Assert.False(settings.NoteModel.NoteModelEnabled, "greyed while the lane loads");
        Assert.True(settings.NoteModel.ModelLoading);
        Assert.False(settings.NoteModel.PickerEnabled);
        Assert.Equal(
            "Loading the note model · 0:00 · this can take a few minutes",
            settings.NoteModel.NoteModelCaption);
        // A switch the user asked for says so on the status bar, with the time it has taken
        Assert.Equal("Switching to Qwen3.5 4B · 0:00", status.LatestActivity);
        Assert.True(status.Busy);

        engine.RaiseNotification("note/model", NoteModel("loading", "constrained", "Qwen3.5 4B"));
        Assert.False(settings.NoteModel.NoteModelEnabled);

        engine.RaiseNotification("note/model", NoteModel("ready", "constrained", "Qwen3.5 4B"));
        Assert.True(settings.NoteModel.NoteModelEnabled);
        Assert.Equal("", settings.NoteModel.NoteModelStatus);
        Assert.StartsWith("Larger models", settings.NoteModel.NoteModelCaption);
        Assert.Equal(0, settings.NoteModel.NoteModelIndex);
        // The status bar's busy line ends with the load
        Assert.Equal("Ready", status.LatestActivity);
        Assert.False(status.Busy);
    }

    // A reconnect re-reads the store. Only a changed store rebuilds the bound
    // collection under the control
    [Fact]
    public void AReconnectWithTheSameModelsLeavesTheCollectionAlone()
    {
        var engine = TieredEngine();
        var settings = new SettingsViewModel(TempPreferences(), client: new EngineApi(engine), session: new FakeSession());
        var changes = 0;
        settings.NoteModel.NoteModelOptions.CollectionChanged += (_, _) => changes++;
        settings.NoteModel.NoteModelIndex = 2;

        engine.SetConnected(false);
        engine.SetConnected(true);

        Assert.Equal(0, changes);
        Assert.Equal(2, settings.NoteModel.NoteModelIndex);

        engine.ExtraNoteModels.RemoveAt(1);  // the 4B was uninstalled
        engine.SetConnected(false);
        engine.SetConnected(true);

        Assert.True(changes > 0);
        Assert.Equal(["Qwen3.5 9B", "Qwen3.6 35B"], settings.NoteModel.NoteModelOptions);
        Assert.Equal(1, settings.NoteModel.NoteModelIndex);
    }

    [Fact]
    public void ASingleStagedModelLeavesNothingToChooseAndAnUninstalledPreferenceFallsBack()
    {
        var settings = new SettingsViewModel(TempPreferences(), client: new EngineApi(new FakeEngineClient()));

        Assert.Equal(["Qwen3.5 9B"], settings.NoteModel.NoteModelOptions);
        Assert.Equal(0, settings.NoteModel.NoteModelIndex);
        Assert.False(settings.NoteModel.NoteModelEnabled);
        Assert.Equal("Only one model installed", settings.NoteModel.NoteModelStatus);

        var preferences = TempPreferences();
        preferences.NoteTier = "accuracy";
        var uninstalled = new SettingsViewModel(preferences, client: new EngineApi(new FakeEngineClient()));

        Assert.Equal("auto", preferences.NoteTier);
        Assert.Equal(0, uninstalled.NoteModel.NoteModelIndex);
        Assert.Contains("not installed", uninstalled.NoteModel.NoteModelStatus);
    }

    [Fact]
    public void AFailedLoadOrARefusedRequestRevertsToTheTierThatWorked()
    {
        var preferences = TempPreferences();
        var engine = TieredEngine();
        var settings = new SettingsViewModel(preferences, client: new EngineApi(engine), session: new FakeSession());
        settings.NoteModel.NoteModelIndex = 2;
        engine.Requests.Clear();

        engine.RaiseNotification(
            "note/model", NoteModel("failed", "accuracy", "Qwen3.6 35B", "out of memory"));

        Assert.Equal("auto", preferences.NoteTier);
        Assert.Equal(1, settings.NoteModel.NoteModelIndex);
        Assert.Contains("out of memory", settings.NoteModel.NoteModelStatus);
        var back = engine.Requests.Single(r => r.Method == "note/tier");
        Assert.Contains("auto", back.Params);
        engine.RaiseNotification("note/model", NoteModel("ready", "default", "Qwen3.5 9B"));

        engine.FailNext = method => method == "note/tier" ? new IOException("no such device") : null;
        settings.NoteModel.NoteModelIndex = 0;

        Assert.Equal("auto", preferences.NoteTier);
        Assert.Equal(1, settings.NoteModel.NoteModelIndex);
        Assert.Contains("no such device", settings.NoteModel.NoteModelStatus);
    }

    // Refused while a note is being written: the resident model never changed, so nothing is
    // sent again and the load that began optimistically ends
    [Fact]
    public void ARefusedSwitchEndsTheLoadWithoutAskingAgain()
    {
        var preferences = TempPreferences();
        var engine = TieredEngine();
        var status = TestSession.Status(engine);
        var settings = new SettingsViewModel(preferences, client: new EngineApi(engine), session: new FakeSession(), status: status);
        engine.Failing.Add("note/tier");

        settings.NoteModel.NoteModelIndex = 0;

        Assert.Single(engine.Requests, r => r.Method == "note/tier");
        Assert.False(settings.NoteModel.ModelLoading);
        Assert.True(settings.NoteModel.PickerEnabled);
        Assert.Equal(1, settings.NoteModel.NoteModelIndex);
        Assert.Equal("auto", preferences.NoteTier);
        Assert.StartsWith("Could not switch", settings.NoteModel.NoteModelStatus);
    }

    [Fact]
    public void TheEngineIsAuthoritativeAboutWhatIsResident()
    {
        var preferences = TempPreferences();
        preferences.NoteTier = "accuracy";
        var engine = TieredEngine();
        var settings = new SettingsViewModel(preferences, client: new EngineApi(engine), session: new FakeSession());

        // The control follows a tier set by another shell instance
        engine.RaiseNotification("note/model", NoteModel("ready", "constrained", "Qwen3.5 4B"));

        Assert.Equal(0, settings.NoteModel.NoteModelIndex);
        Assert.Equal("constrained", preferences.NoteTier);
    }

    // Never chosen: the engine picks for the machine, and the control shows its pick without
    // saving it, so a machine that changes is picked for again
    [Fact]
    public void TheEnginesOwnPickIsShownAndNeverSavedAsAChoice()
    {
        var preferences = TempPreferences();
        var engine = TieredEngine();
        engine.NoteTier = "constrained";
        var settings = new SettingsViewModel(preferences, client: new EngineApi(engine), session: new FakeSession());

        Assert.Equal(0, settings.NoteModel.NoteModelIndex);
        engine.RaiseNotification("note/model", NoteModel("ready", "default", "Qwen3.5 9B"));
        Assert.Equal(1, settings.NoteModel.NoteModelIndex);
        Assert.Equal("auto", preferences.NoteTier);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "note/tier");

        // Choosing one saves it
        settings.NoteModel.NoteModelIndex = 2;
        Assert.Equal("accuracy", preferences.NoteTier);
    }

    private sealed class FixedMachine : ClinicAVT.App.Core.Ports.IMachineInfoProvider
    {
        public ClinicAVT.App.Core.Metrics.MachineInfo Describe() =>
            new("TestCpu", 32, "TestOs", [], null);
    }

    [Fact]
    public async Task ExportExplainsAnEmptyLogThenGoesWhereThePickerChoseOrTheDefaultFolder()
    {
        var empty = new SettingsViewModel(machine: new FixedMachine());
        empty.Appearance.ExportPerformanceReportCommand.Execute(null);
        Assert.Equal("no performance data collected yet", empty.Appearance.ExportResult);

        var dir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(dir);
        var collector = new ClinicAVT.App.Core.Metrics.PerformanceCollector(
            new EngineApi(new FakeEngineClient()), () => true, () => null, Path.Combine(dir, "metrics.jsonl"));
        collector.SessionStarted("mic", 0, null);
        collector.StopRequested();
        await collector.SessionFinishedAsync(null, 10);
        var picker = new FakeFilePicker();
        var settings = new SettingsViewModel(machine: new FixedMachine(), metrics: collector, picker: picker);
        var chosen = Path.Combine(dir, "picked.html");

        await settings.Appearance.ExportPerformanceReportCommand.ExecuteAsync(null);
        Assert.Equal("", settings.Appearance.ExportResult);  // cancel is silent
        Assert.False(File.Exists(chosen));

        picker.SavePath = chosen;
        await settings.Appearance.ExportPerformanceReportCommand.ExecuteAsync(null);
        Assert.True(File.Exists(chosen), "written where the picker chose");
        Assert.Contains("picked.html", settings.Appearance.ExportResult);
        Assert.Contains("TestCpu", File.ReadAllText(chosen));
        Directory.Delete(dir, recursive: true);
    }

    [Fact]
    public void ThemeDefaultsToSystemPersistsAndAppliesLive()
    {
        var preferences = TempPreferences();
        var theme = new FakeThemeService();
        var settings = new SettingsViewModel(preferences, theme: theme);
        Assert.Equal("system", settings.Appearance.Theme);
        Assert.Equal(0, settings.Appearance.ThemeIndex);

        settings.Appearance.ThemeIndex = 2;

        Assert.Equal("dark", settings.Appearance.Theme);
        Assert.Equal(["dark"], theme.Applied);
        Assert.Equal("dark", preferences.Theme);

        var reopened = new SettingsViewModel(preferences);
        Assert.Equal(2, reopened.Appearance.ThemeIndex);
    }

    [Fact]
    public void MetricsChipsDefaultOffAndPersistWhileDeveloperToolsCloseOnEveryLaunch()
    {
        var path = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        var preferences = new AppPreferences(path);
        var bar = TestSession.Status();
        var settings = new SettingsViewModel(preferences, status: bar);
        Assert.False(settings.Appearance.ShowPerformanceMetrics, "chips are for testing, not GPs");
        Assert.False(bar.MetricsVisible);
        Assert.False(settings.Appearance.DeveloperToolsExpanded);

        settings.Appearance.ShowPerformanceMetrics = true;
        settings.Appearance.DeveloperToolsExpanded = true;

        Assert.True(bar.MetricsVisible);
        Assert.True(preferences.ShowPerformanceMetrics);

        var relaunched = new SettingsViewModel(AppPreferences.Load(path));
        Assert.True(relaunched.Appearance.ShowPerformanceMetrics);
        Assert.False(relaunched.Appearance.DeveloperToolsExpanded);
    }

    [Fact]
    public void InstalledCorporaFollowTheEngineAndARefusedOneKeepsItsPlace()
    {
        var engine = new FakeEngineClient();
        var settings = new SettingsViewModel(TempPreferences(), client: new EngineApi(engine));

        var fixture = Assert.Single(settings.Guidance.GuidanceCorpora);
        Assert.Equal("Fixture guidance corpus", fixture.Name);
        Assert.Equal("40 passages · 11 Sep 2026", fixture.Detail);
        Assert.Equal("none", fixture.Attribution);
        Assert.False(fixture.Refused);
        Assert.Equal("", settings.Guidance.GuidanceCaption);
        Assert.False(settings.Guidance.GuidanceCaptionVisible);
        Assert.True(settings.Guidance.GuidanceInstalledVisible);

        // The embedder is reloaded and comes back with a different store
        engine.GuidanceState = "loading";
        engine.GuidanceCorpora.Clear();
        engine.RaiseNotification("guidance/model");
        Assert.Empty(settings.Guidance.GuidanceCorpora);
        Assert.Equal("Loading", settings.Guidance.GuidanceCaption);

        engine.GuidanceState = "ready";
        engine.GuidanceCorpora.Add(new
        {
            name = "NICE guidance",
            licence = "OGL v3",
            attribution = "Contains public sector information",
            chunks = 22991,
            builtAt = "2026-09-11T21:03:17Z",
        });
        engine.GuidanceCorpora.Add(new
        {
            id = "nice-2026-08",
            unavailable = "corpus.db sha256 does not match the manifest",
        });
        engine.RaiseNotification("guidance/model");

        Assert.Equal(2, settings.Guidance.GuidanceCorpora.Count);
        var nice = settings.Guidance.GuidanceCorpora[0];
        Assert.Equal("22,991 passages · 11 Sep 2026", nice.Detail);
        Assert.Equal("Contains public sector information", nice.Attribution);
        var refused = settings.Guidance.GuidanceCorpora[1];
        Assert.Equal("nice-2026-08", refused.Name);
        Assert.Equal("Not used: corpus.db sha256 does not match the manifest", refused.Detail);
        Assert.True(refused.Refused);
        Assert.False(refused.Loaded);
        Assert.Equal("", settings.Guidance.GuidanceCaption);
    }

    [Fact]
    public void NothingInstalledHidesTheCardAndAnUnavailableModelSaysWhy()
    {
        var empty = new FakeEngineClient { GuidanceState = "ready" };
        empty.GuidanceCorpora.Clear();
        var settings = new SettingsViewModel(TempPreferences(), client: new EngineApi(empty));
        Assert.Empty(settings.Guidance.GuidanceCorpora);
        Assert.Equal("", settings.Guidance.GuidanceCaption);
        Assert.False(settings.Guidance.GuidanceInstalledVisible);

        empty.GuidanceState = "unavailable";
        empty.GuidanceDetail = "guidance embedder gte-large-int8: tokenizer ignores max_length";
        empty.RaiseNotification("guidance/model");

        Assert.Empty(settings.Guidance.GuidanceCorpora);
        Assert.Equal(
            "Unavailable: guidance embedder gte-large-int8: tokenizer ignores max_length",
            settings.Guidance.GuidanceCaption);
        Assert.True(settings.Guidance.GuidanceCaptionVisible);
    }

    private static object Document(long id, string name, string state, int chunks = 0,
        string? error = null, int pages = 0, int pagesWithoutText = 0) => new
        {
            id,
            name,
            path = name + ".txt",
            sha256 = new string('0', 64),
            mime = "text/plain",
            state,
            error,
            addedAt = "2026-09-15T09:12:44Z",
            indexedAt = state == "ready" ? "2026-09-15T09:13:02Z" : null,
            bytes = 1000,
            pages,
            pagesWithoutText,
            chunks,
        };

    [Fact]
    public void AddedDocumentsListWorkingRowsFirstThenUnreadableThenByName()
    {
        var engine = new FakeEngineClient();
        engine.GuidanceDocuments.Add(Document(1, "Gout", "ready", 41));
        engine.GuidanceDocuments.Add(Document(2, "asthma", "ready", 12));
        engine.GuidanceDocuments.Add(Document(3, "PMR", "indexing"));
        engine.GuidanceDocuments.Add(Document(4, "Letter", "failed", error: "patientData"));
        var settings = new SettingsViewModel(TempPreferences(), client: new EngineApi(engine));

        Assert.Equal(["PMR", "Letter", "asthma", "Gout"], settings.Guidance.Documents.Select(d => d.Name));
        Assert.Equal("Waiting", settings.Guidance.Documents[0].Detail);
        Assert.True(settings.Guidance.Documents[0].Waiting);
        Assert.StartsWith("Not searched: this looks like a document about a patient.",
            settings.Guidance.Documents[1].Detail);
        Assert.True(settings.Guidance.Documents[1].Failed);
        Assert.Equal("12 passages · added 15 Sep 2026", settings.Guidance.Documents[2].Detail);
        Assert.Equal("4 documents · reading 1, 1 could not be read", settings.Guidance.DocumentsSummary);
        Assert.True(settings.Guidance.DocumentsExpanded, "a failure opens the list");
        Assert.True(settings.Guidance.DocumentsPresent);
        Assert.True(settings.Guidance.AddDocumentsCommand.CanExecute(null));
    }

    [Fact]
    public async Task AddingDocumentsSendsThePathsAndCountsWhatWasSkipped()
    {
        var engine = new FakeEngineClient();
        engine.AddedDocuments.Add(Document(5, "PMR pathway", "indexing"));
        engine.SkippedDocuments.Add(new { path = @"C:\g\scan.pdf", reason = "unsupported" });
        engine.SkippedDocuments.Add(new { path = @"C:\g\empty.txt", reason = "unreadable" });
        var picker = new FakeFilePicker
        {
            Files = [@"C:\g\PMR pathway.txt", @"C:\g\scan.pdf", @"C:\g\empty.txt"],
        };
        var settings = new SettingsViewModel(TempPreferences(), client: new EngineApi(engine), picker: picker);

        await settings.Guidance.AddDocumentsCommand.ExecuteAsync(null);

        var request = Assert.Single(engine.Requests, r => r.Method == "guidance/documents/add");
        Assert.Contains("PMR pathway.txt", request.Params);
        var row = Assert.Single(settings.Guidance.Documents);
        Assert.Equal("PMR pathway", row.Name);
        Assert.True(row.Working);
        Assert.Equal("1 skipped, not PDF or text · 1 could not be read", settings.Guidance.DocumentsCaption);
    }

    [Fact]
    public async Task RowsFollowTheEngineAndRemoveAlwaysConfirmsBecauseItBinsTheFile()
    {
        var engine = new FakeEngineClient();
        engine.GuidanceDocuments.Add(Document(3, "PMR", "indexing"));
        var asked = 0;
        var dialogs = new FakeDialogService { OnConfirm = () => asked++ };
        var settings = new SettingsViewModel(TempPreferences(), client: new EngineApi(engine), dialogs: dialogs);
        var row = Assert.Single(settings.Guidance.Documents);

        engine.RaiseNotification("guidance/progress",
            Params(new { id = 3, phase = "paused", done = 0, total = 10 }));
        Assert.Equal("Waiting for the consultation to finish", row.Detail);
        engine.RaiseNotification("guidance/progress",
            Params(new { id = 3, phase = "preparing", done = 3, total = 10 }));
        Assert.Equal("Preparing 3 of 10 passages", row.Detail);
        Assert.Equal(0.3, row.Progress, 3);
        Assert.False(row.Waiting);

        // Removing a document that is still being read asks first, since it bins the file
        await settings.Guidance.RemoveDocumentCommand.ExecuteAsync(row);
        Assert.Equal(1, asked);
        Assert.Contains(engine.Requests,
            r => r.Method == "guidance/documents/remove" && r.Params == "{\"id\":3}");

        engine.RaiseNotification("guidance/document", Params(Document(3, "PMR", "ready", 10)));
        Assert.Equal("10 passages · added 15 Sep 2026", row.Detail);
        Assert.False(row.Working);
        Assert.DoesNotContain("·", settings.Guidance.DocumentsSummary);

        await settings.Guidance.RemoveDocumentCommand.ExecuteAsync(row);
        Assert.Equal(2, asked);

        engine.RaiseNotification("guidance/document", Params(Document(3, "PMR", "removed", 10)));
        Assert.Empty(settings.Guidance.Documents);
        Assert.False(settings.Guidance.DocumentsPresent);
    }

    [Fact]
    public void TheFolderIsListedAndAddableEvenBeforeTheGuidanceModelLoads()
    {
        var engine = new FakeEngineClient { GuidanceState = "loading" };
        engine.GuidanceDocuments.Add(Document(1, "Gout", "ready", 41));
        var settings = new SettingsViewModel(TempPreferences(), client: new EngineApi(engine));

        // Copying a file into the folder needs no embedder, so Add is always available
        Assert.True(settings.Guidance.AddDocumentsCommand.CanExecute(null));
        Assert.Single(settings.Guidance.Documents);
        Assert.Equal(@"C:\Users\clinician\Documents\ClinicAVT guidelines", settings.Guidance.GuidelinesFolder);
        Assert.False(settings.Guidance.FolderMissing);
    }

    [Fact]
    public void OneDriveFolderIsRecognised()
    {
        var roots = new[] { @"C:\Users\p\OneDrive", @"C:\Users\p\OneDrive - UCL" };
        Assert.True(GuidanceLibrary.InOneDrive(@"C:\Users\p\onedrive - ucl\Documents\ClinicAVT guidelines", roots));
        Assert.False(GuidanceLibrary.InOneDrive(@"C:\Users\p\Documents\ClinicAVT guidelines", roots));
        Assert.False(GuidanceLibrary.InOneDrive("", roots));
    }

    [Fact]
    public void AnUnreachableFolderIsFlaggedAndNothingIsRemoved()
    {
        var engine = new FakeEngineClient { GuidelinesFolderFound = false, UnsupportedFiles = 2 };
        engine.GuidanceDocuments.Add(Document(1, "Gout", "ready", 41));
        var settings = new SettingsViewModel(TempPreferences(), client: new EngineApi(engine));

        Assert.True(settings.Guidance.FolderMissing);
        Assert.Single(settings.Guidance.Documents);
        Assert.Equal("2 other files are not searched, not PDF or text", settings.Guidance.DocumentsCaption);
    }
}
