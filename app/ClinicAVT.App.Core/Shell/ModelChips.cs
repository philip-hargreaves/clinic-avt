using System.ComponentModel;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Shell;

/// <summary>
/// The testing chips: the models doing the clinician's work with their live numbers, and the
/// memory the product holds.
/// </summary>
public sealed partial class ModelChips : ObservableObject
{
    private readonly IEngineControl _engine;
    private readonly ConsultationActivity _activity;
    private readonly AppPreferences _preferences;
    private readonly IProcessMetrics _processes;
    private readonly IStatusLine _line;
    private readonly TimeProvider _time;
    private readonly ThroughputMeter _meter = new();
    private readonly long _started;

    private bool _polling;
    private bool _decoding;
    private double _frozenRealtime;
    private string _asrName = "";
    private string _noteName = "";
    private string _asrDevice = "";
    private string _noteDevice = "";
    // The lane has named its model since this connection began. The model list, fetched
    // before the shell sends its saved tier, would name the engine's default instead
    private bool _laneNamed;

    /// <summary>
    /// The bar meters generation live. Whichever lane streams sends one partial per token, so
    /// the number moves with every token. Without a memory probe there is no memory chip.
    /// </summary>
    public ModelChips(
        IEngineControl engine, IEngineEvents events, ModelActivity models, ConsultationActivity activity,
        AppPreferences preferences, IProcessMetrics processes, IStatusLine line, TimeProvider time)
    {
        _engine = engine;
        _activity = activity;
        _preferences = preferences;
        _processes = processes;
        _line = line;
        _time = time;
        _started = _time.GetTimestamp();
        MetricsVisible = preferences.ShowPerformanceMetrics;
        preferences.Saved += () => MetricsVisible = _preferences.ShowPerformanceMetrics;
        events.OnConnected(() =>
        {
            _ = LoadModelsAsync();
            StartPolling();
        });
        events.SubscribeConnection(connected =>
        {
            if (!connected)
            {
                _laneNamed = false;
            }
        });
        events.Subscribe<EngineNotification>(OnNotification);
        // A tier switch changes which model the chip names. The notification
        // carries the name, so the chip is right from the start of the load and
        // even when the store call behind it times out on a busy engine
        models.Named += name =>
        {
            _noteName = name;
            _laneNamed = true;
            RecomputeChips();
        };
        models.Resident += () => _ = LoadModelsAsync();
        activity.PropertyChanged += OnActivityChanged;
        activity.Started += ResetThroughput;
    }

    /// <summary>Rolling tokens per second. Holds its last value after a stream ends.</summary>
    [ObservableProperty]
    public partial double TokensPerSecond { get; private set; }

    [ObservableProperty]
    public partial bool TokensStreaming { get; private set; }

    /// <summary>Transcription speed as a multiple of real time, 0 when unknown.</summary>
    [ObservableProperty]
    public partial double RealtimeFactor { get; private set; }

    /// <summary>The chips are for testing, and Settings can hide them.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(AsrChipVisible), nameof(NoteChipVisible), nameof(MemoryChipVisible))]
    public partial bool MetricsVisible { get; private set; }

    // The two models doing the clinician's work, each with its live number, such as
    // "Whisper Turbo · GPU · 33× RT" and "Qwen3.5 9B · GPU · 14.2 tok/s"
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(AsrChipVisible))]
    public partial string AsrChip { get; private set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(NoteChipVisible))]
    public partial string NoteChip { get; private set; } = "";

    /// <summary>"Memory · 5.1 GB", the memory the product holds, resident model included.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(MemoryChipVisible))]
    public partial string MemoryChip { get; private set; } = "";

    public bool AsrChipVisible => MetricsVisible && AsrChip.Length > 0;

    public bool NoteChipVisible => MetricsVisible && NoteChip.Length > 0;

    public bool MemoryChipVisible => MetricsVisible && MemoryChip.Length > 0;

    /// <summary>The chip's dot is green while this model is working.</summary>
    public bool AsrActive => _activity.Listening || _activity.Decoding;

    public bool NoteActive => TokensStreaming;

    // The resting dot is the visible-inverse half of the colour pair the view
    // swaps, exposed as a property for XAML binding
    public bool AsrResting => !AsrActive;

    public bool NoteResting => !NoteActive;

    // Memory covers the shell, engine and note host, the whole on-device footprint. The note
    // host is found by name because it is the engine's child process. A failure leaves the
    // last value
    public async Task PollMetricsOnceAsync()
    {
        var memory = await Task.Run(() => _processes.CommittedGb(
            EngineLayout.EngineProcess, EngineLayout.NoteHostProcess)).ConfigureAwait(true);
        MemoryChip = memory > 0
            ? $"Memory · {memory.ToString("0.0", CultureInfo.CurrentCulture)} GB"
            : "";

        if (!_engine.Connected)
        {
            return;
        }

        await EngineCall.LogAsync(_line, "engine/metrics", async () =>
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

    private void OnNotification(EngineNotification notification)
    {
        switch (notification)
        {
            // The translator is not the note model, so its rate never reaches this chip
            case NotePartial or PatientPartial:
                _meter.Token(Now());
                PublishThroughput(SourceRate(notification));
                break;
            case NoteReady or PatientReady:
                _meter.End(Now());
                // The ready event carries the whole-generation average
                PublishThroughput(SourceRate(notification));
                break;
            case NoteFailed or PatientFailed:
                _meter.End(Now());
                PublishThroughput(null);
                break;
            default:
                break;
        }
    }

    private void OnActivityChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(ConsultationActivity.Listening))
        {
            RecomputeChips();
        }
        else if (e.PropertyName == nameof(ConsultationActivity.Decoding))
        {
            if (_decoding && !_activity.Decoding && RealtimeFactor > 0)
            {
                // At the seal the per-session counters make this the session's exact average
                // decode speed, held for reading after the run
                _frozenRealtime = RealtimeFactor;
            }

            _decoding = _activity.Decoding;
            RecomputeChips();
        }
    }

    /// <summary>A new consultation meters from nothing.</summary>
    private void ResetThroughput()
    {
        _meter.Reset();
        _frozenRealtime = 0;
        PublishThroughput();
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

        await EngineCall.LogAsync(_line, "engine/models", async () =>
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
                if (model.Task == ModelTask.Asr && _asrName.Length == 0)
                {
                    (_asrName, _asrDevice) = (name, device);
                }
                else if (model.Task == ModelTask.Note && _noteName.Length == 0)
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
            AsrActive && RealtimeFactor > 0
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
                await Task.Delay(TimeSpan.FromSeconds(AsrActive ? 1 : 5), _time)
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
