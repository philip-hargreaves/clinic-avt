using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>
/// Lists the staged models in tier order, switches between them and reverts a failed switch.
/// The tier stays "auto", showing the engine's pick, until the user chooses.
/// </summary>
public sealed partial class NoteModelSettings : ObservableObject
{
    private readonly AppPreferences _preferences;
    private readonly IEngineControl _engine;
    private readonly ISessionState _session;
    private readonly IStatusLine _status;
    private readonly IModelActivity _models;

    // Tier keys in ladder order, parallel to NoteModelOptions
    private readonly List<string> _tiers = [];
    private string _noteTier;  // the preference, "auto" until one is chosen
    private string? _residentTier;  // what the engine has loaded, as it last said
    private string? _revertTier;  // where a failed switch goes back to
    private bool _populating;
    private bool _reverting;

    public NoteModelSettings(
        AppPreferences preferences, IEngineControl engine, IEngineEvents events, ISessionState session,
        IStatusLine status, IModelActivity models)
    {
        _preferences = preferences;
        _engine = engine;
        _session = session;
        _status = status;
        _models = models;
        _noteTier = preferences.NoteTier;
        models.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(INoteModelLoad.ModelLoadLine))
            {
                OnPropertyChanged(nameof(NoteModelCaption));
                OnPropertyChanged(nameof(ModelLoading));
                OnPropertyChanged(nameof(PickerEnabled));
            }
        };
        events.Subscribe<NoteModelState>(Apply);
        events.OnConnected(Connected);
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

    /// <summary>Disabled during any load, including one started before this page opened.
    /// Loads cannot be interrupted.</summary>
    public bool PickerEnabled => NoteModelEnabled && !ModelLoading;

    /// <summary>Lane status, empty when nothing is happening.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(NoteModelCaption))]
    public partial string NoteModelStatus { get; set; } = "";

    public string NoteModelCaption =>
        ModelLoading ? _models.ModelLoadLine
        : string.IsNullOrEmpty(NoteModelStatus) ? "Larger models are more accurate and use more memory."
        : NoteModelStatus;

    public bool ModelLoading => _models.ModelLoading;

    private bool Automatic => _noteTier == AppPreferences.AutoNoteTier;

    // The choice, or while automatic the engine's pick
    private string? ShownTier => Automatic ? _residentTier : _noteTier;

    private void Connected() => _ = LoadNoteModelsAsync();

    private void Apply(NoteModelState model) =>
        ApplyNoteModel(model.State, model.Tier, model.Detail ?? "");

    private async Task LoadNoteModelsAsync()
    {
        if (!_engine.Connected)
        {
            return;
        }

        await EngineCall.LogAsync(_status, "engine/models", async () =>
        {
            var ladder = AppPreferences.NoteTiers.ToList();
            var models = await _engine.ListModelsAsync().ConfigureAwait(true);
            _residentTier = models.FirstOrDefault(m => m.Task == ModelTask.Note && m.Active)?.Tier ?? _residentTier;
            var staged = models
                .Where(m => m.Task == ModelTask.Note && ladder.Contains(m.Tier))
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
            // For a saved tier that is not staged the engine starts on its own pick, and the
            // choice goes back to automatic
            if (!Automatic && !_tiers.Contains(_noteTier))
            {
                NoteModelStatus = "Saved model not installed; chose one for this computer";
                _noteTier = AppPreferences.AutoNoteTier;
                PersistTier();
            }

            NoteModelIndex = ShownTier is { } shown ? _tiers.IndexOf(shown) : -1;
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
        if (tier == ShownTier)
        {
            return;
        }

        // Switching unloads the model a consultation needs
        if (ConsultationGuard.Blocks(_session, _status, "changing the note model"))
        {
            Reselect(ShownTier);
            return;
        }

        _revertTier = _noteTier;
        _noteTier = tier;
        PersistTier();
        NoteModelEnabled = false;
        NoteModelStatus = "";
        _models.ApplyNoteModel(ModelState.Loading, firstUse: false);
        _models.BeginSwitch($"Switching to {NoteModelOptions[value]} · {{time}}");
        _ = SendTierAsync(tier);
    }

    // Moves the control's selection without treating it as a switch
    private void Reselect(string? tier)
    {
        _reverting = true;
        NoteModelIndex = tier is null ? -1 : _tiers.IndexOf(tier);
        _reverting = false;
    }

    private void PersistTier() => _preferences.Update(p => p.NoteTier = _noteTier);

    private async Task SendTierAsync(string tier)
    {
        try
        {
            var reply = await _engine.SetNoteTierAsync(tier).ConfigureAwait(true);
            _models.ApplyNoteModel(reply.State, firstUse: null, reply.Name);
            // Nothing to wait for when the tier is already resident, as after a restart
            if (reply.State == ModelState.Ready)
            {
                ApplyNoteModel(reply.State, reply.Tier, detail: "");
            }
        }
        catch (Exception e)
        {
            RefuseTier(e.Message);
        }
    }

    // A refusal leaves the resident model unchanged, so the selection is restored and the
    // optimistic load ends. Nothing is resent
    private void RefuseTier(string reason)
    {
        var back = _revertTier;
        _revertTier = null;
        _models.ApplyNoteModel(ModelState.Failed, firstUse: null);
        _models.EndSwitch();
        NoteModelStatus = $"Could not switch: {reason}";
        _status.Append($"Could not switch note model: {reason}");
        if (back is not null)
        {
            _noteTier = back;
            PersistTier();
            Reselect(ShownTier);
        }

        NoteModelEnabled = _tiers.Count > 1;
    }

    // A failed load reverts to the previous tier, once, and the engine is told
    private void RevertTier(string reason)
    {
        var back = _revertTier;
        _revertTier = null;
        _models.EndSwitch();
        // With no switch pending, the current model itself failed
        NoteModelStatus = back is null ? reason : $"Could not switch: {reason}";
        _status.Append(back is null ? $"Note model: {reason}" : $"Could not switch note model: {reason}");
        if (back is null)
        {
            NoteModelEnabled = _tiers.Count > 1;
            return;
        }

        _noteTier = back;
        PersistTier();
        Reselect(ShownTier);
        _ = SendTierAsync(back);
    }

    private void ApplyNoteModel(ModelState state, string tier, string detail)
    {
        switch (state)
        {
            case ModelState.Loading:
                if (Automatic)
                {
                    _residentTier = tier;
                }

                // The caption shows the load's own line while it runs
                NoteModelEnabled = false;
                NoteModelStatus = "";
                break;
            case ModelState.Ready:
                _models.EndSwitch();
                // A revert lands on the model still resident, whose ready must not wipe the
                // reason the switch failed
                if (_revertTier is not null)
                {
                    _status.Append("Ready");
                    NoteModelStatus = "";
                }

                _revertTier = null;
                NoteModelEnabled = _tiers.Count > 1;
                // The engine decides what is resident. Its own pick is shown and is not saved
                // as a choice
                _residentTier = tier;
                if (Automatic)
                {
                    Reselect(tier);
                }
                else if (_tiers.Contains(tier) && tier != _noteTier)
                {
                    _noteTier = tier;
                    PersistTier();
                    Reselect(tier);
                }

                break;
            case ModelState.Failed:
                if (tier == _noteTier || (Automatic && tier == _residentTier))
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
