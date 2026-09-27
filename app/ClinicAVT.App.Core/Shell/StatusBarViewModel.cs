using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Shell;

/// <summary>
/// The status line, its busy ring, the activity log and the testing chips. The chips show the
/// models with their live numbers and the product's memory. Clinician-facing text carries no
/// engine, model or process vocabulary.
/// </summary>
public sealed partial class StatusBarViewModel : ObservableObject
{
    private readonly IEngineApi _engine;
    private readonly IUiDispatcher _dispatcher;
    private readonly ILogger? _logger;
    private readonly TimeProvider _time;
    private readonly ThroughputMeter _meter = new();
    private readonly Func<double>? _memoryGb;
    private readonly long _started;

    private EngineStatus _status = EngineStatus.Stopped;
    private bool _ready;
    private bool _activityBusy;
    private bool _polling;
    private double _frozenRealtime;
    private string _asrName = "";
    private string _noteName = "";
    private string _asrDevice = "";
    private string _noteDevice = "";
    private DateTimeOffset _loadSince;
    // The lane has named its model since this connection began. The model list, fetched
    // before the shell sends its saved tier, would name the engine's default instead
    private bool _laneNamed;
    private bool _preparing;
    // First-time setup holds recording while the models compile for this computer
    private bool _settingUp;
    private DateTimeOffset _setupSince;
    private ITimer? _tick;
    // The store stopped taking writes. It stays on the line until the next consultation starts
    private string _storageFault = "";
    // Nothing is being recorded, finalised or reviewed, so the next step is a recording
    private bool _sessionIdle = true;

    /// <summary>
    /// The bar meters generation live. Whichever lane streams sends one partial per token, so
    /// the number moves with every token. Without a memory probe there is no memory chip.
    /// </summary>
    public StatusBarViewModel(IEngineApi engine, IUiDispatcher dispatcher,
        TimeProvider? time = null, Func<double>? memoryGb = null, ILogger? logger = null)
    {
        _engine = engine;
        _logger = logger;
        _time = time ?? TimeProvider.System;
        _memoryGb = memoryGb;
        _started = _time.GetTimestamp();
        _dispatcher = dispatcher;
        engine.OnConnected(dispatcher, () =>
        {
            _ = LoadModelsAsync();
            StartPolling();
        });
        // A load in progress is lost with the engine that was running it
        engine.ConnectedChanged += connected => dispatcher.Post(() =>
        {
            if (!connected)
            {
                EndModelLoad();
                _laneNamed = false;
            }
        });

        engine.NotificationReceived += notification => dispatcher.Post(() =>
        {
            switch (notification)
            {
                case NotePartial or PatientPartial or TranslationPartial:
                    _meter.Token(Now());
                    PublishThroughput(SourceRate(notification));
                    break;
                case NoteReady or PatientReady or TranslationReady:
                    _meter.End(Now());
                    // The ready event carries the whole-generation average
                    PublishThroughput(SourceRate(notification));
                    break;
                case NoteFailed or PatientFailed or TranslationFailed:
                    _meter.End(Now());
                    PublishThroughput(null);
                    break;
                // A tier switch changes which model the chip names. The notification
                // carries the name, so the chip is right from the start of the load and
                // even when the store call behind it times out on a busy engine
                case NoteModelState model:
                    ApplyNoteModel(model.State, model.FirstUse, model.Name);
                    if (model.State == "ready")
                    {
                        _ = LoadModelsAsync();
                    }

                    break;
                case StorageFault fault:
                    ShowStorageFault(fault.Detail);
                    break;
                default:
                    break;
            }
        });
    }

    [ObservableProperty]
    public partial string EngineStateLabel { get; private set; } = "Starting";

    /// <summary>True in every transient state. The status ring spins on it.</summary>
    [ObservableProperty]
    public partial bool EngineStarting { get; private set; }

    [ObservableProperty]
    public partial string LatestActivity { get; private set; } = "";

    /// <summary>
    /// True when demo mode is on or a demo record is on screen. It shows beside the app name.
    /// </summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DemoLabel))]
    public partial bool Demo { get; set; }

    [ObservableProperty]
    public partial double MicLevel { get; private set; }

    /// <summary>The level bar shows only while audio is flowing.</summary>
    [ObservableProperty]
    public partial bool MicVisible { get; private set; }

    /// <summary>Rolling tokens per second. Holds its last value after a stream ends.</summary>
    [ObservableProperty]
    public partial double TokensPerSecond { get; private set; }

    [ObservableProperty]
    public partial bool TokensStreaming { get; private set; }

    /// <summary>Transcription speed as a multiple of real time, 0 when unknown.</summary>
    [ObservableProperty]
    public partial double RealtimeFactor { get; private set; }

    /// <summary>
    /// True from stop until the sealed transcript loads. The finalise tail decode is the NPU's
    /// longest stage, so the RT figure stays on screen through it.
    /// </summary>
    [ObservableProperty]
    public partial bool DecodeActive { get; private set; }

    /// <summary>The chips are for testing and stay off unless opted in.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(AsrChipVisible), nameof(NoteChipVisible), nameof(MemoryChipVisible))]
    public partial bool MetricsVisible { get; set; }

    // The two models doing the clinician's work, each with its live number, such as
    // "Whisper Turbo · GPU · 33× RT" and "Qwen3.5 9B · GPU · 14.2 tok/s"
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(AsrChipVisible))]
    public partial string AsrChip { get; private set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(NoteChipVisible))]
    public partial string NoteChip { get; private set; } = "";

    /// <summary>"Memory · 5.1 GB", the product's whole working set.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(MemoryChipVisible))]
    public partial string MemoryChip { get; private set; } = "";

    // One status on screen, replaced as things happen. A storage fault outranks everything
    // while the engine runs, abnormal readiness outranks activity, and activity outranks Ready.
    // A note model loading behind a ready app is not shown here: nothing waits on it, and the
    // places that do wait say so themselves
    public string DisplayLabel =>
        _status == EngineStatus.Running && _storageFault.Length > 0 ? _storageFault
        : _status == EngineStatus.Running && ShowsSetup ? SetupLine
        : !_ready || _status != EngineStatus.Running ? EngineStateLabel
        : LatestActivity.Length > 0 ? LatestActivity
        : EngineStateLabel;

    // Setup shows its own bar in place of the ring
    public bool Busy => !ShowsSetup && (EngineStarting || _activityBusy);

    public bool ShowsSetup => _settingUp && !MicVisible;

    /// <summary>Time since first-time setup began, as 2:10.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SetupLine))]
    [NotifyPropertyChangedFor(nameof(DisplayLabel))]
    public partial string SetupElapsed { get; private set; } = "";

    /// <summary>The consent reminder, shown only while a recording could start.</summary>
    public bool ConsentVisible => _status == EngineStatus.Running && _ready && _sessionIdle && !_settingUp;

    public string SetupLine =>
        $"Setting up for this computer · {SetupElapsed} · this can take a few minutes";

    /// <summary>A note model is loading. Loads can take minutes, so the line counts the time.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ModelLoadLine))]
    public partial bool ModelLoading { get; private set; }

    /// <summary>Time since the load began, as 0:48.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ModelLoadLine))]
    public partial string ModelLoadElapsed { get; private set; } = "";

    public string ModelLoadLine => ModelLoading
        ? $"{(_preparing ? "Preparing the note model for this computer" : "Loading the note model")} · {ModelLoadElapsed} · this can take a few minutes"
        : "";

    /// <summary>The note lane's state as the engine reports it, in a notification or a reply.
    /// A reply does not say whether the load is a first use, so it passes null.</summary>
    public void ApplyNoteModel(string state, bool? firstUse, string? name = null)
    {
        if (!string.IsNullOrWhiteSpace(name) && state is "loading" or "ready")
        {
            _noteName = name;
            _laneNamed = true;
            RecomputeChips();
        }

        // A switch passes through idle between the old model and the new one, so only an
        // outcome ends a load
        if (state is "ready" or "failed")
        {
            EndModelLoad();
            return;
        }

        if (state != "loading")
        {
            return;
        }

        _preparing = firstUse ?? _preparing;
        if (ModelLoading)
        {
            OnPropertyChanged(nameof(ModelLoadLine));
            return;
        }

        _loadSince = _time.GetUtcNow();
        ModelLoadElapsed = "0:00";
        ModelLoading = true;
        Tick();
    }

    /// <summary>Recording is held while the models compile for this computer.</summary>
    public void SetSettingUp(bool settingUp)
    {
        if (settingUp == _settingUp)
        {
            return;
        }

        _settingUp = settingUp;
        if (settingUp)
        {
            _setupSince = _time.GetUtcNow();
            SetupElapsed = "0:00";
            _logger?.Line("first-time setup: compiling the models for this computer");
        }

        Tick();
        OnPropertyChanged(nameof(ShowsSetup));
        OnPropertyChanged(nameof(DisplayLabel));
        OnPropertyChanged(nameof(Busy));
        OnPropertyChanged(nameof(ConsentVisible));
    }

    // One clock for both counters, running only while one of them counts
    private void Tick()
    {
        if (ModelLoading || _settingUp)
        {
            _tick ??= _time.CreateTimer(
                _ => _dispatcher.Post(OnTick), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
        }
        else
        {
            _tick?.Dispose();
            _tick = null;
        }
    }

    private void OnTick()
    {
        var now = _time.GetUtcNow();
        if (ModelLoading)
        {
            ModelLoadElapsed = Words.Clock((now - _loadSince).TotalSeconds);
        }

        if (_settingUp)
        {
            SetupElapsed = Words.Clock((now - _setupSince).TotalSeconds);
        }
    }

    private void EndModelLoad()
    {
        ModelLoading = false;
        Tick();
    }

    public string DemoLabel => Demo ? "Demo" : "";

    public bool AsrChipVisible => MetricsVisible && AsrChip.Length > 0;

    public bool NoteChipVisible => MetricsVisible && NoteChip.Length > 0;

    public bool MemoryChipVisible => MetricsVisible && MemoryChip.Length > 0;

    /// <summary>The chip's dot is green while this model is working.</summary>
    public bool AsrActive => MicVisible || DecodeActive;

    public bool NoteActive => TokensStreaming;

    // The resting dot is the visible-inverse half of the colour pair the view
    // swaps, exposed as a property for XAML binding
    public bool AsrResting => !AsrActive;

    public bool NoteResting => !NoteActive;

    public void SetEngineState(EngineStatus status)
    {
        _status = status;
        Recompute();

        // Silent restarts stay out of the activity log. Faults go in
        if (status == EngineStatus.Faulted)
        {
            Append(EngineStateLabel);
        }
    }

    public void SetEngineReady(bool ready)
    {
        _ready = ready;
        Recompute();
    }

    /// <summary>Log-only detail, to the file. The displayed status stays concise.</summary>
    public void Log(string line) => _logger?.Line(line);

    /// <summary>Sets the status line and logs it.</summary>
    public void Append(string line, bool busy = false)
    {
        _logger?.Line(line);
        Show(line, busy);
    }

    /// <summary>Sets the status line without logging it, for a figure that ticks over.</summary>
    public void Show(string line, bool busy = false)
    {
        _activityBusy = busy;
        LatestActivity = line;
        OnPropertyChanged(nameof(DisplayLabel));
        OnPropertyChanged(nameof(Busy));
    }

    public void SetMicLevel(double level) => MicLevel = level;

    public void SetSessionIdle(bool idle)
    {
        _sessionIdle = idle;
        OnPropertyChanged(nameof(ConsentVisible));
    }

    public void SetMicVisible(bool visible)
    {
        MicVisible = visible;
        OnPropertyChanged(nameof(ShowsSetup));
        OnPropertyChanged(nameof(DisplayLabel));
        OnPropertyChanged(nameof(Busy));
        RecomputeChips();
        if (!visible)
        {
            SetMicLevel(0);
        }
    }

    /// <summary>A new consultation starts with the storage fault line cleared.</summary>
    public void ClearStorageFault()
    {
        if (_storageFault.Length > 0)
        {
            _storageFault = "";
            OnPropertyChanged(nameof(DisplayLabel));
        }
    }

    /// <summary>A new consultation meters from nothing.</summary>
    public void ResetThroughput()
    {
        _meter.Reset();
        _frozenRealtime = 0;
        PublishThroughput();
    }

    public void SetDecodeActive(bool active)
    {
        if (DecodeActive && !active && RealtimeFactor > 0)
        {
            // At the seal the per-session counters make this the session's exact average
            // decode speed, held for reading after the run
            _frozenRealtime = RealtimeFactor;
        }

        DecodeActive = active;
        RecomputeChips();
    }

    // Memory covers the shell, engine and note host, the whole on-device footprint. The note
    // host is found by name because it is the engine's child process. A failure leaves the
    // last value
    public async Task PollMetricsOnceAsync()
    {
        var memory = _memoryGb is null ? 0 : await Task.Run(_memoryGb).ConfigureAwait(true);
        MemoryChip = memory > 0
            ? $"Memory · {memory.ToString("0.0", CultureInfo.CurrentCulture)} GB"
            : "";

        if (!_engine.Connected)
        {
            return;
        }

        await EngineCall.LogAsync(this, "engine/metrics", async () =>
        {
            var metrics = await _engine.MetricsAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(true);
            if (metrics.AsrRealtimeFactor is { } factor)
            {
                RealtimeFactor = factor;
            }

            if (metrics.Devices is { } devices)
            {
                _asrDevice = ShortDevice(devices.Asr ?? _asrDevice);
                _noteDevice = ShortDevice(devices.Note ?? _noteDevice);
                RecomputeChips();
            }
        }).ConfigureAwait(true);
    }

    partial void OnRealtimeFactorChanged(double value) => RecomputeChips();

    private void ShowStorageFault(string detail)
    {
        _storageFault = detail.Length > 0
            ? $"Can't save to disk: {detail}. Recording continues; free some space."
            : "Can't save to disk. Recording continues; free some space.";
        _logger?.Line(_storageFault);
        OnPropertyChanged(nameof(DisplayLabel));
    }

    private void Recompute()
    {
        EngineStarting = _status == EngineStatus.Running && !_ready
            || _status == EngineStatus.Restarting;
        EngineStateLabel = _status switch
        {
            EngineStatus.Running when !_ready => "Starting up",
            EngineStatus.Running => "Ready",
            EngineStatus.Restarting => "Recovering",
            EngineStatus.Faulted => "Recording is unavailable - please restart the app",
            _ => "Not running",
        };
        OnPropertyChanged(nameof(DisplayLabel));
        OnPropertyChanged(nameof(Busy));
        OnPropertyChanged(nameof(ConsentVisible));
    }

    private double Now() => _time.GetElapsedTime(_started).TotalSeconds;

    private void PublishThroughput(double? sourceRate = null)
    {
        TokensPerSecond = sourceRate ?? _meter.TokensPerSecond(Now());
        TokensStreaming = _meter.Streaming;
        RecomputeChips();
    }

    private async Task LoadModelsAsync()
    {
        if (!_engine.Connected)
        {
            return;
        }

        await EngineCall.LogAsync(this, "engine/models", async () =>
        {
            var models = await _engine.ListModelsAsync().ConfigureAwait(true);
            // The chip names the model the engine marks active for the role. Without that flag
            // the default tier is assumed
            var laneName = _noteName;
            _asrName = _noteName = "";
            foreach (var model in models.OrderBy(m => m.Active ? 0 : m.Tier == "default" ? 1 : 2))
            {
                var name = ModelNames.Display(model);
                var device = ShortDevice(model.Device);
                if (model.Task == "asr" && _asrName.Length == 0)
                {
                    (_asrName, _asrDevice) = (name, device);
                }
                else if (model.Task == "note" && _noteName.Length == 0)
                {
                    (_noteName, _noteDevice) = (name, device);
                }
            }

            if (_laneNamed && laneName.Length > 0)
            {
                _noteName = laneName;
            }
        }).ConfigureAwait(true);

        RecomputeChips();
        await PollMetricsOnceAsync().ConfigureAwait(true);  // reported devices beat manifests
    }

    // Live figures are unlabelled and move. A settled one says "Averaged" and is the session's
    // true average, held through review for reading after a run
    private void RecomputeChips()
    {
        OnPropertyChanged(nameof(AsrActive));
        OnPropertyChanged(nameof(NoteActive));
        OnPropertyChanged(nameof(AsrResting));
        OnPropertyChanged(nameof(NoteResting));
        AsrChip = Chip(_asrName, _asrDevice,
            (MicVisible || DecodeActive) && RealtimeFactor > 0
                ? $"{Figure(RealtimeFactor)}× RT"
                : _frozenRealtime > 0 ? $"Averaged {Figure(_frozenRealtime)}× RT" : "");
        var tok = TokensPerSecond.ToString("0.0", CultureInfo.CurrentCulture);
        NoteChip = Chip(_noteName, _noteDevice,
            TokensPerSecond <= 0 ? ""
            : TokensStreaming ? $"{tok} tok/s"
            : $"Averaged {tok} tok/s");

        static string Figure(double value) => value.ToString(
            value < 10 ? "0.0" : "0", CultureInfo.CurrentCulture);

        static string Chip(string name, string device, string figure)
        {
            if (name.Length == 0)
            {
                return "";
            }

            var chip = device.Length > 0 ? $"{name} · {device}" : name;
            return figure.Length > 0 ? $"{chip} · {figure}" : chip;
        }
    }

    private void StartPolling()
    {
        if (!_polling)
        {
            _ = PollWhileConnectedAsync();
        }
    }

    // One loop for the connected lifetime. It polls at 1 Hz while recording and every 5 s when
    // idle, so a device switch reaches the chip within seconds
    private async Task PollWhileConnectedAsync()
    {
        _polling = true;
        try
        {
            while (_engine.Connected)
            {
                await PollMetricsOnceAsync().ConfigureAwait(true);
                await Task.Delay(TimeSpan.FromSeconds(MicVisible || DecodeActive ? 1 : 5), _time)
                    .ConfigureAwait(true);
            }
        }
        finally
        {
            _polling = false;  // reconnection starts a fresh loop
        }
    }

    // The engine measures at the source, before its notification throttle,
    // so its figure beats the local arrival count whenever it is present
    private static double? SourceRate(EngineNotification notification) =>
        (notification as IMetered)?.TokensPerSecond;

    private static string ShortDevice(string device) =>
        device.Split('.')[0];  // the device index is a build detail
}
