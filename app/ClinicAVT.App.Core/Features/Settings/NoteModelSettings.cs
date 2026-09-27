using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>
/// The note model tier. It lists the staged models as a ladder, makes the switch, and takes a
/// failed switch back. The engine's store resolves a tier to a model.
/// </summary>
public sealed partial class NoteModelSettings : ObservableObject
{
    private readonly AppPreferences? _preferences;
    private readonly IEngineApi? _client;
    private readonly ISessionState? _session;
    private readonly StatusBarViewModel? _status;

    // Tier keys in ladder order, parallel to NoteModelOptions
    private readonly List<string> _tiers = [];
    private string _noteTier;
    private string? _revertTier;  // where a failed switch goes back to
    private bool _populating;
    private bool _reverting;

    public NoteModelSettings(
        AppPreferences? preferences, IEngineApi? client, ISessionState? session, StatusBarViewModel? status)
    {
        _preferences = preferences;
        _client = client;
        _session = session;
        _status = status;
        _noteTier = preferences?.NoteTier ?? "default";
        if (status is not null)
        {
            status.PropertyChanged += (_, e) =>
            {
                if (e.PropertyName is nameof(StatusBarViewModel.ModelLoadLine))
                {
                    OnPropertyChanged(nameof(NoteModelCaption));
                    OnPropertyChanged(nameof(ModelLoading));
                    OnPropertyChanged(nameof(PickerEnabled));
                }
            };
        }
    }

    /// <summary>Display names of the staged note models, smallest first.</summary>
    public ObservableCollection<string> NoteModelOptions { get; } = [];

    /// <summary>The chosen note model as the control's selection.</summary>
    [ObservableProperty]
    public partial int NoteModelIndex { get; set; } = -1;

    /// <summary>False while the lane loads, and when there is nothing to choose between.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PickerEnabled))]
    public partial bool NoteModelEnabled { get; set; }

    /// <summary>A load cannot be interrupted, so the picker waits for any load, including one
    /// that began before this page did.</summary>
    public bool PickerEnabled => NoteModelEnabled && !ModelLoading;

    /// <summary>Lane status, empty when nothing is happening.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(NoteModelCaption))]
    public partial string NoteModelStatus { get; set; } = "";

    public string NoteModelCaption =>
        ModelLoading ? _status!.ModelLoadLine
        : string.IsNullOrEmpty(NoteModelStatus) ? "Larger models are more accurate and use more memory."
        : NoteModelStatus;

    /// <summary>A load is running: the picker waits and a bar shows under it.</summary>
    public bool ModelLoading => _status?.ModelLoading == true;

    /// <summary>On connect the options load from the engine's store.</summary>
    public void Connected() => _ = LoadNoteModelsAsync();

    /// <summary>The lane's state drives the control.</summary>
    public void Apply(NoteModelState model) =>
        ApplyNoteModel(model.State, model.Tier, model.Detail ?? "");

    private async Task LoadNoteModelsAsync()
    {
        if (!_client.IsConnected())
        {
            return;
        }

        await EngineCall.LogAsync(_status, "engine/models", async () =>
        {
            var ladder = AppPreferences.NoteTiers.ToList();
            var models = await _client.ListModelsAsync().ConfigureAwait(true);
            var staged = models
                .Where(m => m.Task == "note" && ladder.Contains(m.Tier))
                .Select(m => (m.Tier, Name: ModelNames.Display(m)))
                .OrderBy(m => ladder.IndexOf(m.Tier))
                .ToList();

            _populating = true;
            // Rebuilt only when the store's contents differ. Clearing ComboBox items under an
            // open popup or a live selection can fault in XAML, and every reconnect would do it
            // otherwise
            var tiers = staged.Select(m => m.Tier).ToList();
            var names = staged.Select(m => m.Name).ToList();
            if (!tiers.SequenceEqual(_tiers) || !names.SequenceEqual(NoteModelOptions))
            {
                NoteModelIndex = -1;  // clear the selection before the items
                _tiers.Clear();
                NoteModelOptions.Clear();
                foreach (var (tier, name) in staged)
                {
                    _tiers.Add(tier);
                    NoteModelOptions.Add(name);
                }
            }

            NoteModelStatus = _tiers.Count <= 1 ? "Only one model installed" : "";
            // For a saved tier that is not staged the engine stays on the default, and the
            // control shows that
            if (!_tiers.Contains(_noteTier) && _tiers.Contains("default"))
            {
                NoteModelStatus = "Saved model not installed; using the default";
                _noteTier = "default";
                PersistTier();
            }

            NoteModelIndex = _tiers.IndexOf(_noteTier);
            NoteModelEnabled = _tiers.Count > 1;
        }).ConfigureAwait(true);
        _populating = false;
    }

    partial void OnNoteModelIndexChanged(int value)
    {
        if (_reverting || _populating || value < 0 || value >= _tiers.Count)
        {
            return;
        }

        var tier = _tiers[value];
        if (tier == _noteTier)
        {
            return;
        }

        // The switch ends the resident model. A consultation needs it
        if (ConsultationGuard.Blocks(_session, _status, "changing the note model"))
        {
            Reselect(_noteTier);
            return;
        }

        _revertTier = _noteTier;
        _noteTier = tier;
        PersistTier();
        NoteModelEnabled = false;
        NoteModelStatus = "";
        _status?.ApplyNoteModel("loading", firstUse: false);
        _ = SendTierAsync(tier);
    }

    // Moves the control's selection without treating it as a switch
    private void Reselect(string tier)
    {
        _reverting = true;
        NoteModelIndex = _tiers.IndexOf(tier);
        _reverting = false;
    }

    private void PersistTier() => _preferences.Update(p => p.NoteTier = _noteTier);

    private async Task SendTierAsync(string tier)
    {
        if (_client is null)
        {
            return;
        }

        try
        {
            var reply = await _client.SetNoteTierAsync(tier).ConfigureAwait(true);
            _status?.ApplyNoteModel(reply.State, firstUse: null, reply.Name);
            // Nothing to wait for when the tier is already resident, as after a restart
            if (reply.State == "ready")
            {
                ApplyNoteModel(reply.State, reply.Tier, detail: "");
            }
        }
        catch (Exception e)
        {
            RevertTier(e.Message);
        }
    }

    // A refused or failed switch reverts to the previous tier, once, and the
    // engine is told
    private void RevertTier(string reason)
    {
        var back = _revertTier;
        _revertTier = null;
        // Without a switch in hand this is the model failing where it is
        NoteModelStatus = back is null ? reason : $"Could not switch: {reason}";
        _status?.Append(back is null ? $"Note model: {reason}" : $"Could not switch note model: {reason}");
        if (back is null)
        {
            NoteModelEnabled = _tiers.Count > 1;
            return;
        }

        _noteTier = back;
        PersistTier();
        Reselect(back);
        _ = SendTierAsync(back);
    }

    private void ApplyNoteModel(string state, string tier, string detail)
    {
        switch (state)
        {
            case "loading":
                // The caption shows the load's own line while it runs
                NoteModelEnabled = false;
                NoteModelStatus = "";
                break;
            case "ready":
                // A switch in flight puts a busy line on the status bar. The ready state ends it.
                // A revert lands on the model still resident, whose ready must not wipe the
                // reason the switch failed
                if (_revertTier is not null)
                {
                    _status?.Append("Ready");
                    NoteModelStatus = "";
                }

                _revertTier = null;
                NoteModelEnabled = _tiers.Count > 1;
                // The engine is authoritative about what is resident
                if (_tiers.Contains(tier) && tier != _noteTier)
                {
                    _noteTier = tier;
                    PersistTier();
                    Reselect(tier);
                }

                break;
            case "failed":
                if (tier == _noteTier)
                {
                    RevertTier(detail);
                }
                else
                {
                    NoteModelStatus = $"Load failed: {detail}";
                }

                break;
            default:
                break;
        }
    }
}
