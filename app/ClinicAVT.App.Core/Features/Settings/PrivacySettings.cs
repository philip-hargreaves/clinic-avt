using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Backup;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>Privacy settings: history switch, erase all and sample data.</summary>
public sealed partial class PrivacySettings : ObservableObject
{
    private readonly AppPreferences _preferences;
    private readonly ISessionStoreApi _store;
    private readonly IArchiveApi _archive;
    private readonly ISessionState _session;
    private readonly IStatusLine _status;
    private readonly IDialogService _dialogs;
    private readonly Func<BackupViewModel> _backups;
    private readonly Func<RestoreViewModel> _restores;
    private readonly bool _initialising;
    private bool _reverting;

    // Set while the switch is aligned to the store, so the engine is not asked again
    private bool _seedFollowsStore;

    public PrivacySettings(
        AppPreferences preferences, ISessionStoreApi store, IArchiveApi archive, IEngineEvents events,
        ISessionState session, IStatusLine status, IDialogService dialogs, Func<BackupViewModel> backups,
        Func<RestoreViewModel> restores)
    {
        _preferences = preferences;
        _store = store;
        _archive = archive;
        _session = session;
        _status = status;
        _dialogs = dialogs;
        _backups = backups;
        _restores = restores;
        // Restoring saved values is not the clinician changing them
        _initialising = true;
        KeepConsultations = preferences.KeepConsultations;
        SeedDataEnabled = preferences.SeedDataEnabled;
        _initialising = false;
        // A backup made from the Sessions page changes the Back up card's line too
        preferences.Saved += () => OnPropertyChanged(nameof(BackupDescription));
        events.OnConnected(Connected);
    }

    /// <summary>Reseeds sample data on connect if on.</summary>
    private void Connected()
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

        if (value)
        {
            SetKeepConsultationsQuietly(false);  // holds until the clinician confirms
            _ = AskThenEnableAsync();
            return;
        }

        PersistKeepConsultations(value);
    }

    private async Task AskThenEnableAsync()
    {
        if (await _dialogs.ConfirmAsync("Save consultation data?",
                "Transcripts, notes and patient information will be stored encrypted on this device."
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

    /// <summary>The Back up card's line: what a backup is, and when the last one was made.</summary>
    public string BackupDescription =>
        "Save consultations to a password-protected file. "
        + (_preferences.LastBackup is { } last
            ? $"Last backup: {Words.ShortDate(last.CreatedAt)}, {Words.Count(last.Consultations, "consultation")}."
            : "No backup yet.");

    [RelayCommand]
    private Task BackUp() => RunDialogAsync("backing up", () => _dialogs.RunBackupAsync(_backups));

    [RelayCommand]
    private Task Restore() => RunDialogAsync("restoring", () => _dialogs.RunRestoreAsync(_restores));

    private async Task RunDialogAsync(string action, Func<Task> run)
    {
        if (!_store.Connected || ConsultationGuard.Blocks(_session, _status, action))
        {
            return;
        }

        await run().ConfigureAwait(true);
    }

    /// <summary>
    /// Erases every stored consultation, seeded or real. Reflections stay with their case summary
    /// unless the clinician ticks them too.
    /// </summary>
    [RelayCommand]
    private async Task DeleteAllConsultations()
    {
        if (!_store.Connected)
        {
            return;
        }

        if (ConsultationGuard.Blocks(_session, _status, "deleting stored data"))
        {
            return;
        }

        var coverage = await CoverageLineAsync().ConfigureAwait(true);
        var answer = await _dialogs.ConfirmWithOptionAsync("Delete all consultations from ClinicAVT?",
            coverage + " This can't be undone.",
            BackupWords.ReflectionsTick, "Delete all").ConfigureAwait(true);
        if (answer is not { } deleteReflections)
        {
            return;
        }

        await EngineCall.ReportAsync(_status, "could not delete", async () =>
        {
            // Every consultation goes, including one open for review
            await _session.EndReviewAsync().ConfigureAwait(true);

            var removed = await _store.DeleteAllSessionsAsync(deleteReflections).ConfigureAwait(true);
            _status.Append($"{Words.Count(removed, "consultation")} deleted");
            // The seed was erased too. The switch follows, and switching on reseeds
            _seedFollowsStore = true;
            SeedDataEnabled = false;
            _seedFollowsStore = false;
        }).ConfigureAwait(true);
    }

    // The engine counts, since only it sees consultations recorded or edited after the backup
    private async Task<string> CoverageLineAsync()
    {
        if (_preferences.LastBackup is not { } last)
        {
            return "No consultations are backed up.";
        }

        try
        {
            var summary = await _archive.ArchiveSummaryAsync("", "",
                new ArchiveCoverage(last.From, last.To, last.CreatedAt)).ConfigureAwait(true);
            return summary.Uncovered switch
            {
                0 => "All consultations are backed up.",
                1 => "1 consultation isn't backed up.",
                var n => $"{Words.Count(n, "consultation")} aren't backed up.",
            };
        }
        catch (Exception e)
        {
            _status.Log($"archive/summary failed: {e.Message}");
            return "The last backup could not be checked.";
        }
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
        if (!_store.Connected)
        {
            return;
        }

        await EngineCall.ReportAsync(_status, "seed data", async () =>
        {
            var count = await (enabled ? _store.SeedSamplesAsync() : _store.ClearSamplesAsync())
                .ConfigureAwait(true);
            if (count > 0)
            {
                _status.Append(enabled
                    ? $"{count} sample consultations added"
                    : $"{count} sample consultations removed");
            }
        }).ConfigureAwait(true);
    }
}
