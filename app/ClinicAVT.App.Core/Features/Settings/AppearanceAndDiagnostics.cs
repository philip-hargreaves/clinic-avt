using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Metrics;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>
/// The theme, the transcription device and the developer tools. The tools are the metrics
/// chips, the performance log and its report.
/// </summary>
public sealed partial class AppearanceAndDiagnostics : ObservableObject
{
    private readonly AppPreferences _preferences;
    private readonly IEngineControl _engine;
    private readonly ISessionState _session;
    private readonly IStatusLine _status;
    private readonly IModelActivity _models;
    private readonly IMachineInfoProvider _machine;
    private readonly IMetricsLog _metrics;
    private readonly IFilePicker _picker;
    private readonly ITextFiles _files;
    private readonly IThemeService _theme;
    private readonly TimeProvider _time;
    private readonly bool _initialising;
    private bool _reverting;

    public AppearanceAndDiagnostics(
        AppPreferences preferences, IEngineControl engine, IEngineEvents events, ISessionState session,
        IStatusLine status, IModelActivity models, IMachineInfoProvider machine, IMetricsLog metrics,
        IFilePicker picker, ITextFiles files, IThemeService theme, TimeProvider time)
    {
        _preferences = preferences;
        _engine = engine;
        _session = session;
        _status = status;
        _models = models;
        _machine = machine;
        _metrics = metrics;
        _picker = picker;
        _files = files;
        _theme = theme;
        _time = time;
        // Restoring saved values is not the clinician changing them
        _initialising = true;
        NpuTranscription = preferences.NpuTranscription;
        CollectPerformanceData = preferences.CollectPerformanceData;
        ShowPerformanceMetrics = preferences.ShowPerformanceMetrics;
        Theme = preferences.Theme;
        _initialising = false;
        events.Subscribe<AsrDeviceState>(Apply);
    }

    /// <summary>The shell applies it live.</summary>
    [ObservableProperty]
    public partial AppTheme Theme { get; set; }

    partial void OnThemeChanged(AppTheme value)
    {
        OnPropertyChanged(nameof(ThemeIndex));
        if (_initialising)
        {
            return;
        }

        _theme.Apply(value);
        _preferences.Update(p => p.Theme = value);
    }

    public IReadOnlyList<string> ThemeOptions { get; } = ["System default", "Light", "Dark"];

    /// <summary>
    /// Theme as the Appearance control's selection, which lists the themes in order.
    /// </summary>
    public int ThemeIndex
    {
        get => (int)Theme;
        set => Theme = (AppTheme)Math.Clamp(value, 0, ThemeOptions.Count - 1);
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
        if (_engine.Connected)
        {
            _models.BeginSwitch(value
                ? "Switching to the NPU · {time} · first time may take longer"
                : "Switching to the GPU · {time}");
            _ = MoveSpeechRecognitionAsync(value);
        }
    }

    /// <summary>The engine's word on a move in progress.</summary>
    private void Apply(AsrDeviceState state)
    {
        if (state.State == ModelState.Ready)
        {
            _models.EndSwitch();
            _status.Append("Ready");
        }
        else if (state.State == ModelState.Failed)
        {
            TakeBack(state.Device == AsrDevice.Npu, state.Detail ?? "failed");
        }
    }

    private async Task MoveSpeechRecognitionAsync(bool npu)
    {
        try
        {
            await _engine.SetAsrDeviceAsync(npu ? AsrDevice.Npu : AsrDevice.Gpu).ConfigureAwait(true);
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
        _models.EndSwitch();
        _status.Append($"Could not switch transcription device: {reason}");
    }

    /// <summary>The Developer tools group, closed on every launch.</summary>
    [ObservableProperty]
    public partial bool DeveloperToolsExpanded { get; set; }

    /// <summary>Shows the status-bar model and memory chips. For testing.</summary>
    [ObservableProperty]
    public partial bool ShowPerformanceMetrics { get; set; }

    partial void OnShowPerformanceMetricsChanged(bool value)
    {
        if (_initialising)
        {
            return;
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
        try
        {
            if (_metrics.ReadAll() is not { } lines)
            {
                ExportResult = "no performance data collected yet";
                return;
            }

            var html = ReportBuilder.Build(_machine.Describe(), lines, _time.GetUtcNow());
            var suggested = $"clinicavt-perf-{_machine.MachineName}-{_time.GetLocalNow():yyyyMMdd}.html";
            var path = await _picker.SaveTextAsync(_files, suggested, "HTML report", ".html", html)
                .ConfigureAwait(true);
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
