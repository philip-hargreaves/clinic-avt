using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>What the store keeps. The history switch, the erase and the sample data.</summary>
public sealed partial class PrivacySettings : ObservableObject
{
    private readonly AppPreferences? _preferences;
    private readonly IEngineApi? _client;
    private readonly ISessionState? _session;
    private readonly StatusBarViewModel? _status;
    private readonly IDialogService? _dialogs;
    private readonly bool _initialising;
    private bool _reverting;

    // Set while the switch is aligned to the store, so the engine is not asked again
    private bool _seedFollowsStore;

    public PrivacySettings(
        AppPreferences? preferences, IEngineApi? client, ISessionState? session,
        StatusBarViewModel? status, IDialogService? dialogs)
    {
        _preferences = preferences;
        _client = client;
        _session = session;
        _status = status;
        _dialogs = dialogs;
        // Restoring saved values is not the clinician changing them
        _initialising = true;
        KeepConsultations = preferences?.KeepConsultations ?? false;
        SeedDataEnabled = preferences?.SeedDataEnabled ?? false;
        _initialising = false;
    }

    /// <summary>On connect the seed follows the switch.</summary>
    public void Connected()
    {
        if (SeedDataEnabled)
        {
            _ = ApplySeedDataAsync(true);
        }
    }

    /// <summary>
    /// Off by default, so a consultation is erased when it is left. On keeps an encrypted
    /// history. A change applies to consultations recorded after it. Audio is never kept
    /// either way.
    /// </summary>
    [ObservableProperty]
    public partial bool KeepConsultations { get; set; }

    // Turning the history on starts accumulating patient records, so it is
    // confirmed first. Turning it off is never gated
    partial void OnKeepConsultationsChanged(bool value)
    {
        if (_reverting || _initialising)
        {
            return;
        }

        if (value && _dialogs is not null)
        {
            SetKeepConsultationsQuietly(false);  // holds until the clinician confirms
            _ = AskThenEnableAsync();
            return;
        }

        PersistKeepConsultations(value);
    }

    private async Task AskThenEnableAsync()
    {
        if (await _dialogs!.ConfirmAsync("Save consultation data?",
                "Transcripts, notes and patient sheets will be stored encrypted on this device."
                + "\n\nContinue only if you have the necessary consent and approval.", "Turn on")
            .ConfigureAwait(true))
        {
            SetKeepConsultationsQuietly(true);
            PersistKeepConsultations(true);
        }
    }

    private void SetKeepConsultationsQuietly(bool value)
    {
        _reverting = true;
        KeepConsultations = value;
        _reverting = false;
    }

    private void PersistKeepConsultations(bool value) =>
        _preferences.Update(p => p.KeepConsultations = value);

    /// <summary>Erases every stored consultation, seeded or real.</summary>
    [RelayCommand]
    private async Task DeleteAllConsultations()
    {
        if (!_client.IsConnected())
        {
            return;
        }

        if (ConsultationGuard.Blocks(_session, _status, "deleting stored data"))
        {
            return;
        }

        if (_dialogs is not null && !await _dialogs.ConfirmAsync("Delete all consultation data?",
                "Every stored consultation on this device is erased: transcripts, notes, patient "
                + "sheets and appraisal reflections. Your guideline documents are kept. This cannot "
                + "be undone.", "Delete all").ConfigureAwait(true))
        {
            return;
        }

        await EngineCall.ReportAsync(_status, "could not delete", async () =>
        {
            var removed = await _client.DeleteAllSessionsAsync().ConfigureAwait(true);
            _status?.Append($"{Words.Count(removed, "consultation")} deleted");
            // The seed was erased too. The switch follows, and switching on reseeds
            _seedFollowsStore = true;
            SeedDataEnabled = false;
            _seedFollowsStore = false;
        }).ConfigureAwait(true);
    }

    /// <summary>A year of sample consultations with reflections. A developer control.</summary>
    [ObservableProperty]
    public partial bool SeedDataEnabled { get; set; }

    partial void OnSeedDataEnabledChanged(bool value)
    {
        if (_initialising)
        {
            return;
        }

        _preferences.Update(p => p.SeedDataEnabled = value);
        if (!_seedFollowsStore)
        {
            _ = ApplySeedDataAsync(value);
        }
    }

    // On seeds the store and does nothing when already seeded. Off clears it
    private async Task ApplySeedDataAsync(bool enabled)
    {
        if (!_client.IsConnected())
        {
            return;
        }

        await EngineCall.ReportAsync(_status, "seed data", async () =>
        {
            var count = await (enabled ? _client.SeedDemoAsync() : _client.ClearDemoAsync())
                .ConfigureAwait(true);
            if (count > 0)
            {
                _status?.Append(enabled
                    ? $"{count} sample consultations added"
                    : $"{count} sample consultations removed");
            }
        }).ConfigureAwait(true);
    }
}
