using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Shell;

/// <summary>
/// The waits the status line counts, which are a note model load, a switch the user started and
/// first-time setup. One clock ticks while any of them runs.
/// </summary>
public sealed partial class ModelActivity : ObservableObject, IModelActivity
{
    private readonly IStatusLine _line;
    private readonly IUiDispatcher _dispatcher;
    private readonly TimeProvider _time;
    private DateTimeOffset _loadSince;
    private bool _preparing;
    private DateTimeOffset _setupSince;
    // A switch in progress. Its status line has {time} standing for the elapsed clock
    private string? _switchLine;
    private DateTimeOffset _switchSince;
    private ITimer? _tick;

    public ModelActivity(IEngineEvents events, IStatusLine line, IUiDispatcher dispatcher, TimeProvider time)
    {
        _line = line;
        _dispatcher = dispatcher;
        _time = time;
        events.Subscribe<NoteModelState>(model =>
        {
            ApplyNoteModel(model.State, model.FirstUse, model.Name);
            if (model.State == ModelState.Ready)
            {
                Resident?.Invoke();
            }
        });
        // An engine restart loses any load in progress
        events.SubscribeConnection(connected =>
        {
            if (!connected)
            {
                EndModelLoad();
                EndSwitch();
            }
        });
    }

    /// <summary>Raised with the name of the model the lane is loading or serving.</summary>
    public event Action<string>? Named;

    /// <summary>Raised when a note/model notification reports the lane's model ready.</summary>
    public event Action? Resident;

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

    // First-time setup holds recording while the models compile for this computer
    [ObservableProperty]
    public partial bool SettingUp { get; private set; }

    /// <summary>Time since first-time setup began, as 2:10.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SetupLine))]
    private partial string SetupElapsed { get; set; } = "";

    internal string SetupLine =>
        $"First-time setup · {SetupElapsed} · optimising for your PC";

    public bool Switching => _switchLine is not null;

    /// <summary>The note lane's state as the engine reports it, in a notification or a reply.
    /// A reply does not say whether the load is a first use, so it passes null.</summary>
    public void ApplyNoteModel(ModelState state, bool? firstUse, string? name = null)
    {
        if (!string.IsNullOrWhiteSpace(name) && state is ModelState.Loading or ModelState.Ready)
        {
            Named?.Invoke(name);
        }

        // A switch passes through idle between the old model and the new one, so only an
        // outcome ends a load
        if (state is ModelState.Ready or ModelState.Failed)
        {
            EndModelLoad();
            return;
        }

        if (state != ModelState.Loading)
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

    /// <summary>
    /// A switch the user started, of note model or transcription device. A first switch compiles
    /// for minutes, so the line counts the time until the engine reports the outcome.
    /// </summary>
    public void BeginSwitch(string line)
    {
        _switchLine = line;
        _switchSince = _time.GetUtcNow();
        _line.Append(SwitchLine(0), busy: true);
        Tick();
        OnPropertyChanged(nameof(Switching));
    }

    public void EndSwitch()
    {
        if (_switchLine is null)
        {
            return;
        }

        _switchLine = null;
        Tick();
        OnPropertyChanged(nameof(Switching));
    }

    /// <summary>Recording is held while the models compile for this computer.</summary>
    public void SetSettingUp(bool settingUp)
    {
        if (settingUp == SettingUp)
        {
            return;
        }

        if (settingUp)
        {
            _setupSince = _time.GetUtcNow();
            SetupElapsed = "0:00";
            _line.Log("first-time setup: compiling the models for this computer");
        }

        SettingUp = settingUp;
        Tick();
    }

    private string SwitchLine(double seconds) =>
        _switchLine!.Replace("{time}", Words.Clock(seconds), StringComparison.Ordinal);

    // One clock for every counter, running only while one of them counts
    private void Tick()
    {
        if (ModelLoading || SettingUp || _switchLine is not null)
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

        if (SettingUp)
        {
            SetupElapsed = Words.Clock((now - _setupSince).TotalSeconds);
        }

        if (_switchLine is not null)
        {
            _line.Show(SwitchLine((now - _switchSince).TotalSeconds), busy: true);
        }
    }

    private void EndModelLoad()
    {
        ModelLoading = false;
        Tick();
    }
}
