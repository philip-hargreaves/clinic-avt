using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Backup;

public enum BackupStep
{
    Setup,
    Working,
    Done,
    ConfirmRemove,
    Removed,
}

/// <summary>
/// The Back up dialog: which consultations, the password, where to save, then the engine writes
/// and checks the file. A checked backup offers to remove exactly what it holds.
/// </summary>
public sealed partial class BackupViewModel : ObservableObject, IDisposable
{
    private readonly IEngineApi _engine;
    private readonly IFilePicker _picker;
    private readonly IClipboard _clipboard;
    private readonly ILauncher _launcher;
    private readonly AppPreferences? _preferences;
    private readonly IUiDispatcher? _dispatcher;
    private readonly TimeProvider _clock;
    private readonly Func<string, string?> _environment;
    private readonly ISessionState? _session;
    private int _countVersion;
    private string _folder = "";
    private string _sentFrom = "";
    private string _sentTo = "";
    private IReadOnlyList<string> _ids = [];

    public BackupViewModel(IEngineApi engine, IFilePicker picker, IClipboard clipboard, ILauncher launcher,
        AppPreferences? preferences = null, IUiDispatcher? dispatcher = null, TimeProvider? clock = null,
        Func<string, string?>? environment = null, ISessionState? session = null)
    {
        _session = session;
        _engine = engine;
        _picker = picker;
        _clipboard = clipboard;
        _launcher = launcher;
        _preferences = preferences;
        _dispatcher = dispatcher;
        _clock = clock ?? TimeProvider.System;
        _environment = environment ?? Environment.GetEnvironmentVariable;
        HasLastBackup = preferences?.LastBackup is not null;
        // One password for every backup, so after the first the clinician's own comes first
        UseGenerated = !HasLastBackup;
        Generated = BackupPasswords.Generate();
        PeriodOptions = Enumerable.Range(0, BackupPeriod.Kinds.Count)
            .Select(i => BackupPeriod.Label(i, Today)).ToArray();
        _engine.NotificationReceived += OnNotification;
    }

    public string Caption { get; } = BackupWords.Caption;

    public string PasswordNote { get; } = BackupWords.PasswordNote;

    public string Includes { get; } = BackupWords.Includes;

    public string SameAsLastLine { get; } = BackupWords.SameAsLast;

    public string ReflectionsTickText { get; } = BackupWords.ReflectionsTick;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(InSetup), nameof(Working), nameof(Finished), nameof(ConfirmingRemove),
        nameof(RemovedStep), nameof(PrimaryText), nameof(CloseText), nameof(PrimaryEnabled), nameof(CanStart),
        nameof(BlocksClose))]
    public partial BackupStep Step { get; private set; }

    public bool InSetup => Step == BackupStep.Setup;

    public bool Working => Step == BackupStep.Working;

    public bool Finished => Step == BackupStep.Done;

    public bool ConfirmingRemove => Step == BackupStep.ConfirmRemove;

    public bool RemovedStep => Step == BackupStep.Removed;

    public string PrimaryText => Step switch
    {
        BackupStep.Setup => "Choose where to save",
        BackupStep.ConfirmRemove => "Remove",
        _ => "",
    };

    public string CloseText => Step switch
    {
        BackupStep.Setup => "Cancel",
        BackupStep.Working => "",
        BackupStep.ConfirmRemove => "Cancel",
        _ => "Done",
    };

    public bool PrimaryEnabled => Step == BackupStep.ConfirmRemove || CanStart;

    /// <summary>The job cannot be stopped once the engine has it, so the dialog stays until it ends.</summary>
    public bool BlocksClose => Step == BackupStep.Working;

    /// <summary>True once consultations were removed, so the lists reload.</summary>
    public bool RemovedAny { get; private set; }

    /// <summary>What went wrong, in plain words, under the step it happened in.</summary>
    [ObservableProperty]
    public partial string Error { get; private set; } = "";

    public IReadOnlyList<string> PeriodOptions { get; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ChoosingDates), nameof(CanStart), nameof(PrimaryEnabled))]
    public partial int PeriodIndex { get; set; } = BackupPeriod.EverythingIndex;

    partial void OnPeriodIndexChanged(int value) => _ = CountAsync();

    public bool ChoosingDates => PeriodIndex == BackupPeriod.ChooseDatesIndex;

    [ObservableProperty]
    public partial DateTimeOffset? ChosenFrom { get; set; }

    partial void OnChosenFromChanged(DateTimeOffset? value) => _ = CountAsync();

    [ObservableProperty]
    public partial DateTimeOffset? ChosenTo { get; set; }

    partial void OnChosenToChanged(DateTimeOffset? value) => _ = CountAsync();

    public BackupPeriod Period => BackupPeriod.For(PeriodIndex, Today,
        ChosenFrom is { } from ? DateOnly.FromDateTime(from.Date) : null,
        ChosenTo is { } to ? DateOnly.FromDateTime(to.Date) : null);

    /// <summary>How many finished consultations the period holds, and its dates.</summary>
    [ObservableProperty]
    public partial string CountLine { get; private set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanStart), nameof(PrimaryEnabled))]
    public partial int Count { get; private set; }

    public bool HasLastBackup { get; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(UseOwn), nameof(SameAsLastVisible), nameof(CanStart), nameof(PrimaryEnabled))]
    public partial bool UseGenerated { get; set; }

    /// <summary>The other choice, for the second radio button.</summary>
    public bool UseOwn
    {
        get => !UseGenerated;
        set => UseGenerated = !value;
    }

    public bool SameAsLastVisible => UseOwn && HasLastBackup;

    [ObservableProperty]
    public partial string Generated { get; private set; } = "";

    [ObservableProperty]
    public partial string CopyLine { get; private set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PasswordProblem), nameof(CanStart), nameof(PrimaryEnabled))]
    public partial string OwnPassword { get; set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PasswordProblem), nameof(CanStart), nameof(PrimaryEnabled))]
    public partial string OwnAgain { get; set; } = "";

    public string PasswordProblem => BackupPasswords.Problem(OwnPassword, OwnAgain);

    public bool CanStart => Step == BackupStep.Setup && Count > 0 && Period.Valid
        && (UseGenerated || BackupPasswords.Acceptable(OwnPassword, OwnAgain));

    [ObservableProperty]
    public partial string ProgressText { get; private set; } = "";

    [ObservableProperty]
    public partial double Progress { get; private set; }

    [ObservableProperty]
    public partial string OneDriveLine { get; private set; } = "";

    [ObservableProperty]
    public partial string DoneLine { get; private set; } = "";

    [ObservableProperty]
    public partial string SavedLine { get; private set; } = "";

    [ObservableProperty]
    public partial string RemoveLine { get; private set; } = "";

    [ObservableProperty]
    public partial bool DeleteReflections { get; set; }

    public bool CanRemove => _ids.Count > 0;

    private DateOnly Today => DateOnly.FromDateTime(_clock.GetLocalNow().DateTime);

    private TimeZoneInfo Zone => _clock.LocalTimeZone;

    /// <summary>Counts the default period. The dialog calls it as it opens.</summary>
    public Task LoadAsync() => CountAsync();

    [RelayCommand]
    private void NewPassword()
    {
        Generated = BackupPasswords.Generate();
        CopyLine = "";
    }

    [RelayCommand]
    private async Task Copy() =>
        CopyLine = await _clipboard.CopySecretAsync(Generated).ConfigureAwait(true)
            ? "Copied. It is kept out of clipboard history."
            : "The password could not be copied. Select the words and copy them instead.";

    [RelayCommand]
    private Task Primary() => Step switch
    {
        BackupStep.Setup => StartAsync(),
        BackupStep.ConfirmRemove => RemoveAsync(),
        _ => Task.CompletedTask,
    };

    [RelayCommand]
    private void ShowInFolder() => _launcher.RevealFolder(_folder);

    [RelayCommand]
    private void AskRemove()
    {
        RemoveLine = $"Remove {Words.Count(_ids.Count, "backed-up consultation")} from ClinicAVT?";
        DeleteReflections = false;
        Error = "";
        Step = BackupStep.ConfirmRemove;
    }

    private async Task CountAsync()
    {
        var version = ++_countVersion;
        var period = Period;
        Count = 0;
        if (ChoosingDates && (period.First is null || period.Last is null))
        {
            CountLine = "Choose the first and last day.";
            return;
        }

        if (!period.Valid)
        {
            CountLine = "The last day is before the first.";
            return;
        }

        CountLine = "Counting consultations…";
        try
        {
            var summary = await _engine.ArchiveSummaryAsync(period.From(Zone), period.To(Zone))
                .ConfigureAwait(true);
            if (version != _countVersion)
            {
                return;
            }

            Count = summary.Consultations;
            CountLine = CountText(period, summary);
        }
        catch (Exception e)
        {
            if (version == _countVersion)
            {
                CountLine = $"The consultations could not be counted: {EngineWords.Reason(e)}.";
            }
        }
    }

    private static string CountText(BackupPeriod period, ArchiveSummary summary)
    {
        if (summary.Consultations == 0)
        {
            return period == BackupPeriod.Everything
                ? "There are no finished consultations to back up."
                : "There are no finished consultations in this period.";
        }

        var count = Words.Count(summary.Consultations, "consultation");
        return period == BackupPeriod.Everything ? $"{count} on this computer." : $"{count}, {period.Span()}.";
    }

    private async Task StartAsync()
    {
        if (!CanStart)
        {
            return;
        }

        Error = "";
        var period = Period;
        var path = await _picker.PickSaveAsync(period.FileName(Today), "ClinicAVT backup", ".clinicavt")
            .ConfigureAwait(true);
        if (path is null)
        {
            return;
        }

        _folder = Path.GetDirectoryName(path) ?? "";
        OneDriveLine = BackupWords.OneDriveLine(_folder, _environment);
        SavedLine = $"Saved as {Path.GetFileNameWithoutExtension(path)} in {FolderName(_folder)}.";
        _sentFrom = period.From(Zone);
        _sentTo = period.To(Zone);
        Progress = 0;
        ProgressText = "Starting the backup…";
        Step = BackupStep.Working;
        try
        {
            await _engine.BackUpAsync(_sentFrom, _sentTo, path, UseGenerated ? Generated : OwnPassword)
                .ConfigureAwait(true);
        }
        catch (Exception e)
        {
            Error = BackupWords.Refused("backup", e);
            OneDriveLine = "";
            Step = BackupStep.Setup;
        }
    }

    private async Task RemoveAsync()
    {
        Error = "";
        Step = BackupStep.Working;
        ProgressText = "Removing consultations…";
        try
        {
            // A consultation open for review goes with the rest, so its review ends first
            if (_session?.ReviewedSessionId is { } open && _ids.Contains(open))
            {
                await _session.EndReviewAsync().ConfigureAwait(true);
            }

            var removed = await _engine.RemoveSessionsAsync(_ids, DeleteReflections).ConfigureAwait(true);
            RemovedAny = true;
            DoneLine = $"{Words.Count(removed, "consultation")} removed from this computer.";
            Step = BackupStep.Removed;
        }
        catch (Exception e)
        {
            Error = $"The consultations could not be removed: {EngineWords.Reason(e)}. The backup is kept.";
            Step = BackupStep.Done;
        }
    }

    private void OnNotification(EngineNotification notification) => _dispatcher.PostOrRun(() =>
    {
        if (Step != BackupStep.Working)
        {
            return;
        }

        switch (notification)
        {
            case ArchiveProgress { Job: "backup" } progress:
                Progress = progress.Total > 0 ? (double)progress.Done / progress.Total : 0;
                ProgressText = progress.Phase == "writing"
                    ? $"Backing up {progress.Done} of {Words.Count(progress.Total, "consultation")}…"
                    : "Checking the backup…";
                break;
            case ArchiveDone { Job: "backup" } done:
                Finish(done);
                break;
            case ArchiveFailed { Job: "backup" } failed:
                Error = BackupWords.Failure("backup", failed.Code);
                OneDriveLine = "";
                Step = BackupStep.Setup;
                break;
        }
    });

    private void Finish(ArchiveDone done)
    {
        _ids = done.Ids;
        OnPropertyChanged(nameof(CanRemove));
        Progress = 1;
        DoneLine = $"{Words.Count(done.Consultations, "consultation")} backed up and checked.";
        var createdAt = string.IsNullOrEmpty(done.CreatedAt)
            ? _clock.GetUtcNow().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", System.Globalization.CultureInfo.InvariantCulture)
            : done.CreatedAt;
        _preferences.Update(p => p.LastBackup = new LastBackup(_sentFrom, _sentTo, createdAt, done.Consultations));
        Step = BackupStep.Done;
    }

    // "Documents" for a library folder, the drive for a root such as a USB stick
    private static string FolderName(string folder) =>
        Path.GetFileName(folder.TrimEnd(Path.DirectorySeparatorChar)) is { Length: > 0 } name ? name : folder;

    public void Dispose() => _engine.NotificationReceived -= OnNotification;
}
