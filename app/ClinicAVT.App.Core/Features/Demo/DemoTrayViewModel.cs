using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Demo;

/// <summary>
/// The developer-only replay transport. A clinician never sees it.
/// </summary>
public sealed partial class DemoTrayViewModel : ObservableObject
{
    // Anything over 1x is for smoke tests only
    private static readonly double[] Speeds = [1, 4, 8, 16];

    private readonly ConsultationViewModel _session;
    private readonly IFilePicker _picker;

    // Read once per selection, so progress ticks do not reread the wav header
    private double _durationSeconds;

    public DemoTrayViewModel(
        ConsultationViewModel session, IFilePicker picker, IReadOnlyList<DemoTrack>? tracks = null,
        AppPreferences? preferences = null)
    {
        _session = session;
        _picker = picker;
        Tracks = new List<DemoTrack>(tracks ?? DemoTracks.Load());
        SelectedTrack = Tracks.FirstOrDefault();
        // The tray exists only while the settings toggle says so, and never in a release build
        Visible = BuildFlags.Debug && (preferences?.DemoTrayEnabled ?? false);
        if (preferences is not null)
        {
            preferences.Saved += () => Visible = BuildFlags.Debug && preferences.DemoTrayEnabled;
        }

        _session.PropertyChanged += (_, e) =>
        {
            switch (e.PropertyName)
            {
                case nameof(ConsultationViewModel.State):
                case nameof(ConsultationViewModel.EngineReady):
                    OnPropertyChanged(nameof(IsReplaying));
                    OnPropertyChanged(nameof(Idle));
                    PlayCommand.NotifyCanExecuteChanged();
                    StopCommand.NotifyCanExecuteChanged();
                    TogglePauseCommand.NotifyCanExecuteChanged();
                    break;
                case nameof(ConsultationViewModel.Paused):
                    OnPropertyChanged(nameof(PauseGlyph));
                    break;
                case nameof(ConsultationViewModel.AudioSeconds):
                    OnPropertyChanged(nameof(ProgressFraction));
                    OnPropertyChanged(nameof(ProgressText));
                    break;
                default:
                    break;
            }
        };
    }

    [ObservableProperty]
    public partial bool Visible { get; private set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(TrackName))]
    [NotifyPropertyChangedFor(nameof(ProgressText))]
    [NotifyPropertyChangedFor(nameof(ProgressFraction))]
    [NotifyCanExecuteChangedFor(nameof(PlayCommand))]
    public partial DemoTrack? SelectedTrack { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SpeedLabel))]
    public partial double Speed { get; set; } = 1;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(MonitorGlyph))]
    public partial bool MonitorAudio { get; set; }

    public List<DemoTrack> Tracks { get; }

    public string TrackName => SelectedTrack?.Name ?? "no track";

    public string SpeedLabel => $"{Speed:0}×";

    public string MonitorGlyph => MonitorAudio ? "" : "";  // volume / mute

    public bool IsReplaying => _session.State == SessionState.Recording
        && _session.ActiveReplay is not null;

    /// <summary>Replay controls show only while idle.</summary>
    public bool Idle => _session.State == SessionState.Idle;

    public string PauseGlyph => _session.Paused ? "" : "";  // play / pause

    /// <summary>Delivered audio against the wav's own duration.</summary>
    public double ProgressFraction => _durationSeconds <= 0
        ? 0
        : Math.Min(1.0, _session.AudioSeconds / _durationSeconds);

    public string ProgressText =>
        $"{Words.Clock(_session.AudioSeconds)} / {Words.Clock(_durationSeconds)}";

    [RelayCommand]
    private async Task Browse()
    {
        var path = await _picker.PickFileAsync([".wav"]).ConfigureAwait(true);
        if (path is not null)
        {
            UseTrack(path);
        }
    }

    /// <summary>1 → 4 → 8 → 16 → 1.</summary>
    [RelayCommand]
    private void CycleSpeed()
    {
        var i = Array.IndexOf(Speeds, Speed);
        Speed = Speeds[(i < 0 ? 0 : i + 1) % Speeds.Length];
    }

    [RelayCommand(CanExecute = nameof(CanPlay))]
    private Task Play() => _session.StartRecordingAsync(
        new ReplayRequest(SelectedTrack!.Path, Speed, MonitorAudio));

    private bool CanPlay() => _session.State == SessionState.Idle && _session.EngineReady
        && SelectedTrack is not null;

    [RelayCommand(CanExecute = nameof(IsReplaying))]
    private Task Stop() => _session.StopRecordingAsync();

    [RelayCommand(CanExecute = nameof(IsReplaying))]
    private Task TogglePause() => _session.SetPausedAsync(!_session.Paused);

    /// <summary>A browsed file becomes a selectable track named after itself.</summary>
    public void UseTrack(string path)
    {
        var track = new DemoTrack(Path.GetFileNameWithoutExtension(path), path);
        Tracks.Add(track);
        OnPropertyChanged(nameof(Tracks));
        SelectedTrack = track;
    }

    partial void OnSelectedTrackChanged(DemoTrack? value) =>
        _durationSeconds = value is null ? 0 : DemoTracks.DurationSeconds(value.Path);

    // Mid-replay the toggle takes effect immediately
    partial void OnMonitorAudioChanged(bool value)
    {
        if (IsReplaying)
        {
            _ = _session.SetMonitorAsync(value);
        }
    }
}
