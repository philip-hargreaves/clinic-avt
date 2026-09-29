using Microsoft.Extensions.DependencyInjection;
using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Core.Metrics;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Features.Settings;

public class AppearanceAndDiagnosticsTest
{
    // Low-power mode moves speech recognition inside the running engine, so the note model
    // stays loaded and nothing restarts
    [Fact]
    public void LaunchRestoresQuietlyAndTheNpuToggleSavesAndMovesSpeechRecognitionInPlace()
    {
        var store = new MemoryPreferencesStore();
        var preferences = new AppPreferences(store)
        {
            NpuTranscription = true,
            KeepConsultations = true,
            Theme = AppTheme.Light,
        };
        var engine = new FakeEngineClient();
        var asked = 0;

        var dialogs = new FakeDialogService { Answer = false, OnConfirm = () => asked++ };
        var shell = TestSession.Settings(engine, preferences, new FakeSession(), dialogs);
        var settings = shell.Get<SettingsViewModel>();
        var status = shell.Line;

        Assert.True(settings.Appearance.NpuTranscription);
        Assert.True(settings.Privacy.KeepConsultations);
        Assert.Equal(AppTheme.Light, settings.Appearance.Theme);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "asr/device");  // a launch moves nothing
        Assert.Equal(0, asked);
        Assert.Null(store.Json);  // launch restore must not re-save

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

    private sealed class NamedMachine : IMachineInfoProvider
    {
        public MachineInfo Describe() => new("TestCpu", 32, "TestOs", [], null);

        public string MachineName => "TESTPC";
    }

    [Fact]
    public async Task ExportExplainsAnEmptyLogThenGoesWhereThePickerChoseOrTheDefaultFolder()
    {
        var empty = TestSession.Settings().Get<AppearanceAndDiagnostics>();
        empty.ExportPerformanceReportCommand.Execute(null);
        Assert.Equal("no performance data collected yet", empty.ExportResult);

        var preferences = new AppPreferences(new MemoryPreferencesStore()) { CollectPerformanceData = true };
        var shell = new TestShell(preferences: preferences, configure: services =>
            services.AddSingleton<IMachineInfoProvider, NamedMachine>());
        var appearance = shell.Get<AppearanceAndDiagnostics>();
        var collector = shell.Get<PerformanceCollector>();
        collector.SessionStarted();
        collector.StopRequested();
        await collector.SessionFinishedAsync(null, 10);
        var files = (FakeTextFiles)shell.Get<ITextFiles>();
        const string chosen = @"C:\Users\clinician\Documents\picked.html";

        await appearance.ExportPerformanceReportCommand.ExecuteAsync(null);
        Assert.Equal("", appearance.ExportResult);  // cancel is silent
        Assert.Empty(files.Written);
        Assert.StartsWith("clinicavt-perf-TESTPC-", shell.Picker.SuggestedNames.Single());

        shell.Picker.SavePath = chosen;
        await appearance.ExportPerformanceReportCommand.ExecuteAsync(null);
        Assert.True(files.Written.ContainsKey(chosen), "written where the picker chose");
        Assert.Contains("picked.html", appearance.ExportResult);
        Assert.Contains("TestCpu", files.Written[chosen]);
    }

    [Fact]
    public void ThemeAndMetricsChipsPersistAndApplyLiveWhileDeveloperToolsCloseOnEveryLaunch()
    {
        var store = new MemoryPreferencesStore();
        var shell = TestSession.Settings(preferences: new AppPreferences(store));
        var theme = (FakeThemeService)shell.Get<IThemeService>();
        var settings = shell.Get<AppearanceAndDiagnostics>();
        Assert.Equal(AppTheme.System, settings.Theme);
        Assert.Equal(0, settings.ThemeIndex);
        Assert.True(settings.ShowPerformanceMetrics, "evaluators see the timings from the first run");
        Assert.False(settings.DeveloperToolsExpanded);

        settings.ThemeIndex = 2;
        settings.ShowPerformanceMetrics = false;
        settings.DeveloperToolsExpanded = true;

        Assert.Equal(AppTheme.Dark, settings.Theme);
        Assert.Equal([AppTheme.Dark], theme.Applied);
        Assert.False(shell.Chips.MetricsVisible);

        var relaunched = TestSession.Settings(preferences: AppPreferences.Load(store))
            .Get<AppearanceAndDiagnostics>();
        Assert.Equal(2, relaunched.ThemeIndex);
        Assert.False(relaunched.ShowPerformanceMetrics);
        Assert.False(relaunched.DeveloperToolsExpanded);
    }
}
