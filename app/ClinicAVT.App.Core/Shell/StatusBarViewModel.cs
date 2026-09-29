using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Features.Consultation;

namespace ClinicAVT.App.Core.Shell;

/// <summary>
/// The status line, its busy ring, the activity log and the testing chips. The chips show the
/// models with their live numbers and the product's memory. Clinician-facing text carries no
/// engine, model or process vocabulary.
/// </summary>
public sealed class StatusBarViewModel : ObservableObject
{
    private static readonly string[] LineChanges = [nameof(DisplayLabel), nameof(Busy)];

    private static readonly string[] StateChanges =
        [nameof(DisplayLabel), nameof(Busy), nameof(ShowsSetup), nameof(ConsentVisible)];

    private readonly StatusLine _line;
    private readonly EngineState _engine;
    private readonly ModelActivity _models;
    private readonly ConsultationActivity _activity;

    public StatusBarViewModel(
        StatusLine line, EngineState engine, ModelActivity models, ModelChips chips,
        ConsultationActivity activity)
    {
        _line = line;
        _engine = engine;
        _models = models;
        Chips = chips;
        _activity = activity;
        line.PropertyChanged += (_, _) => Raise(LineChanges);
        engine.PropertyChanged += (_, _) => Raise(StateChanges);
        models.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(ModelActivity.SettingUp) or nameof(ModelActivity.Switching)
                or nameof(ModelActivity.SetupLine))
            {
                Raise(StateChanges);
            }
        };
        activity.PropertyChanged += OnActivityChanged;
    }

    public ModelChips Chips { get; }

    // One status shows at a time. A storage fault while the engine runs comes first, then abnormal
    // readiness, then activity, then Ready. A background note model load is not shown, because the
    // screens that wait on it show their own line
    public string DisplayLabel =>
        _engine.Running && _line.StorageFault.Length > 0 ? _line.StorageFault
        : _engine.Running && ShowsSetup ? _models.SetupLine
        : !_engine.Ready || !_engine.Running ? _engine.EngineStateLabel
        : _line.LatestActivity.Length > 0 ? _line.LatestActivity
        : _engine.EngineStateLabel;

    // Setup shows its own bar in place of the ring
    public bool Busy => !ShowsSetup && (_engine.EngineStarting || _line.ActivityBusy);

    public bool ShowsSetup => _models.SettingUp && !_activity.Listening;

    /// <summary>The consent reminder, shown only while a recording could start.</summary>
    public bool ConsentVisible => _engine.Running && _engine.Ready && _activity.Idle
        && !_models.SettingUp && !_models.Switching;

    /// <summary>"Demo" while a seeded sample is on screen. It shows beside the app name.</summary>
    public string SampleLabel => _activity.ShowingSample ? "Demo" : "";

    private void OnActivityChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(ConsultationActivity.Listening) or nameof(ConsultationActivity.Idle):
                Raise(StateChanges);
                break;
            case nameof(ConsultationActivity.ShowingSample):
                OnPropertyChanged(nameof(SampleLabel));
                break;
        }
    }

    private void Raise(string[] names)
    {
        foreach (var name in names)
        {
            OnPropertyChanged(name);
        }
    }
}
