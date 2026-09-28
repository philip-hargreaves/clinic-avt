using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Metrics;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>
/// The theme, the transcription device and the developer tools. The tools are the demo
/// controls, the metrics chips, the performance log and its report.
/// </summary>
public sealed partial class AppearanceAndDiagnostics : ObservableObject
{
    private readonly AppPreferences? _preferences;
    private readonly IEngineApi? _client;
    private readonly ISessionState? _session;
    private readonly StatusBarViewModel? _status;
    private readonly IMachineInfoProvider? _machine;
    private readonly PerformanceCollector? _metrics;
    private readonly IFilePicker? _picker;
    private readonly IThemeService? _theme;
    private readonly bool _initialising;
    private bool _reverting;

    public AppearanceAndDiagnostics(
        AppPreferences? preferences, IEngineApi? client, ISessionState? session,
        StatusBarViewModel? status, IMachineInfoProvider? machine, PerformanceCollector? metrics,
        IFilePicker? picker, IThemeService? theme)
    {
        _preferences = preferences;
        _client = client;
        _session = session;
        _status = status;
        _machine = machine;
        _metrics = metrics;
        _picker = picker;
        _theme = theme;
        // Restoring saved values is not the clinician changing them
        _initialising = true;
        DemoTrayEnabled = preferences?.DemoTrayEnabled ?? false;
        NpuTranscription = preferences?.NpuTranscription ?? false;
        CollectPerformanceData = preferences?.CollectPerformanceData ?? false;
        ShowPerformanceMetrics = preferences?.ShowPerformanceMetrics ?? true;
        Theme = preferences?.Theme ?? "system";
        _initialising = false;
    }

    /// <summary>"system", "light" or "dark". The shell applies it live.</summary>
    [ObservableProperty]
    public partial string Theme { get; set; } = "system";

    partial void OnThemeChanged(string value)
    {
        OnPropertyChanged(nameof(ThemeIndex));
        if (_initialising)
        {
            return;
        }

        _theme?.Apply(value);
        _preferences.Update(p => p.Theme = value);
    }

    public IReadOnlyList<string> ThemeOptions { get; } = ["System default", "Light", "Dark"];

    /// <summary>
    /// Theme as the Appearance control's selection, which lists the themes in order.
    /// </summary>
    public int ThemeIndex
    {
        get => Math.Max(0, AppPreferences.Themes.ToList().IndexOf(Theme));
        set => Theme = AppPreferences.Themes[Math.Clamp(value, 0, AppPreferences.Themes.Count - 1)];
    }

    /// <summary>Runs transcription on the NPU. A running engine moves it in place.</summary>
    [ObservableProperty]
    public partial bool NpuTranscription { get; set; }

    partial void OnNpuTranscriptionChanged(bool value)
    {
        if (_reverting || _initialising)
        {
            return;
        }

        // A restart would kill a live session
        if (ConsultationGuard.Blocks(_session, _status, "switching transcription device"))
        {
            _reverting = true;
            NpuTranscription = !value;
            _reverting = false;
            return;
        }

        // Saved for the next start. A running engine moves speech recognition in place, and
        // the note model stays loaded
        _preferences.Update(p => p.NpuTranscription = value);
        if (_client.IsConnected())
        {
            _status?.BeginSwitch(value
                ? "Switching to the NPU · {time} · first time may take longer"
                : "Switching to the GPU · {time}");
            _ = MoveSpeechRecognitionAsync(value);
        }
    }

    /// <summary>The engine's word on a move in progress.</summary>
    public void Apply(AsrDeviceState state)
    {
        if (state.State == "ready")
        {
            _status?.EndSwitch();
            _status?.Append("Ready");
        }
        else if (state.State == "failed")
        {
            TakeBack(state.Device == "NPU", state.Detail ?? "failed");
        }
    }

    private async Task MoveSpeechRecognitionAsync(bool npu)
    {
        try
        {
            await _client!.SetAsrDeviceAsync(npu ? "NPU" : "GPU").ConfigureAwait(true);
        }
        catch (Exception e)
        {
            TakeBack(npu, e.Message);
        }
    }

    // A move that could not happen leaves the toggle and the saved choice where they were
    private void TakeBack(bool npu, string reason)
    {
        _reverting = true;
        NpuTranscription = !npu;
        _reverting = false;
        _preferences.Update(p => p.NpuTranscription = !npu);
        _status?.EndSwitch();
        _status?.Append($"Could not switch transcription device: {reason}");
    }

    /// <summary>The Developer tools group, closed on every launch.</summary>
    [ObservableProperty]
    public partial bool DeveloperToolsExpanded { get; set; }

    /// <summary>Shows the replay tray. A developer control, in debug builds only.</summary>
    public bool DemoTrayAvailable { get; } = BuildFlags.Debug;

    /// <summary>Shows the replay tray. A developer control.</summary>
    [ObservableProperty]
    public partial bool DemoTrayEnabled { get; set; }

    partial void OnDemoTrayEnabledChanged(bool value)
    {
        if (!_initialising)
        {
            _preferences.Update(p => p.DemoTrayEnabled = value);
        }
    }

    /// <summary>Shows the status-bar model and memory chips. For testing.</summary>
    [ObservableProperty]
    public partial bool ShowPerformanceMetrics { get; set; }

    partial void OnShowPerformanceMetricsChanged(bool value)
    {
        if (_initialising)
        {
            return;
        }

        if (_status is not null)
        {
            _status.MetricsVisible = value;
        }

        _preferences.Update(p => p.ShowPerformanceMetrics = value);
    }

    /// <summary>Collects performance figures locally, as numbers and device names only.</summary>
    [ObservableProperty]
    public partial bool CollectPerformanceData { get; set; }

    partial void OnCollectPerformanceDataChanged(bool value)
    {
        if (!_initialising)
        {
            _preferences.Update(p => p.CollectPerformanceData = value);
        }
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ExportDescription))]
    public partial string ExportResult { get; private set; } = "";

    /// <summary>The Export row's line, which shows the last outcome once there is one.</summary>
    public string ExportDescription =>
        ExportResult.Length > 0 ? ExportResult : "Saves the report as an HTML file";

    /// <summary>Saves one self-contained HTML file that can be read, emailed or parsed.</summary>
    [RelayCommand]
    private async Task ExportPerformanceReport()
    {
        if (_machine is null || _metrics is null || !File.Exists(_metrics.Path))
        {
            ExportResult = "no performance data collected yet";
            return;
        }

        try
        {
            var html = ReportBuilder.Build(
                _machine.Describe(), File.ReadAllLines(_metrics.Path), DateTimeOffset.UtcNow);
            var suggested = $"clinicavt-perf-{Environment.MachineName}-{DateTime.Now:yyyyMMdd}.html";
            var path = _picker is null ? null
                : await _picker.SaveTextAsync(suggested, "HTML report", ".html", html).ConfigureAwait(true);
            if (path is null)
            {
                return; // cancelled, so no file and no caption
            }

            ExportResult = $"saved {path}";
        }
        catch (Exception e)
        {
            ExportResult = $"export failed: {e.Message}";
        }
    }
}
