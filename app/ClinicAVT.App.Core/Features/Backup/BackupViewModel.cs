using System.Globalization;
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

/// <summary>The Back up dialog. A checked backup offers to remove the consultations it holds.</summary>
public sealed partial class BackupViewModel : ObservableObject, IDisposable
{
    private readonly IEngineApi _engine;
    private readonly IFilePicker _picker;
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

    public BackupViewModel(IEngineApi engine, IFilePicker picker, ILauncher launcher,
        AppPreferences? preferences = null, IUiDispatcher? dispatcher = null, TimeProvider? clock = null,
        Func<string, string?>? environment = null, ISessionState? session = null)
    {
        _engine = engine;
        _picker = picker;
        _launcher = launcher;
        _preferences = preferences;
        _dispatcher = dispatcher;
        _clock = clock ?? TimeProvider.System;
        _environment = environment ?? Environment.GetEnvironmentVariable;
        _session = session;
        PeriodOptions = Enumerable.Range(0, BackupPeriod.Kinds.Count)
            .Select(i => BackupPeriod.Label(i, Today)).ToArray();
        _engine.NotificationReceived += OnNotification;
    }

    public string PasswordNote { get; } = BackupWords.PasswordNote;

    public string Includes => ReflectionsOnly ? BackupWords.ReflectionsIncludes : BackupWords.Includes;

    public string PeriodHeader => ReflectionsOnly ? "Which reflections" : "Which consultations";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Whole), nameof(Includes), nameof(PeriodHeader))]
    public partial bool ReflectionsOnly { get; set; }

    partial void OnReflectionsOnlyChanged(bool value) => _ = CountAsync();

    /// <summary>The other choice, for the first radio button.</summary>
    public bool Whole
    {
        get => !ReflectionsOnly;
        set => ReflectionsOnly = !value;
    }

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
        BackupStep.Setup or BackupStep.ConfirmRemove => "Cancel",
        BackupStep.Working => "",
        _ => "Done",
    };

    public bool PrimaryEnabled => Step == BackupStep.ConfirmRemove || CanStart;

    /// <summary>The job cannot be stopped once the engine has it, so the dialog stays until it ends.</summary>
    public bool BlocksClose => Step == BackupStep.Working;

    /// <summary>True once consultations were removed, so the lists reload.</summary>
    public bool RemovedAny { get; private set; }

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

    /// <summary>How many the period holds, and its dates.</summary>
    [ObservableProperty]
    public partial string CountLine { get; private set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanStart), nameof(PrimaryEnabled))]
    public partial int Count { get; private set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PasswordProblem), nameof(CanStart), nameof(PrimaryEnabled))]
    public partial string Password { get; set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PasswordProblem), nameof(CanStart), nameof(PrimaryEnabled))]
    public partial string PasswordAgain { get; set; } = "";

    public string PasswordProblem => BackupPasswords.Problem(Password, PasswordAgain);

    public bool CanStart => Step == BackupStep.Setup && Count > 0 && Period.Valid
        && BackupPasswords.Acceptable(Password, PasswordAgain);

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

    /// <summary>Counts the default period as the dialog opens.</summary>
    public Task LoadAsync() => CountAsync();

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

        CountLine = $"Counting {Things(ReflectionsOnly)}s…";
        try
        {
            var summary = await _engine.ArchiveSummaryAsync(period.From(Zone), period.To(Zone))
                .ConfigureAwait(true);
            if (version != _countVersion)
            {
                return;
            }

            Count = ReflectionsOnly ? summary.Reflections : summary.Consultations;
            CountLine = CountText(period, Count, ReflectionsOnly);
        }
        catch (Exception e)
        {
            if (version == _countVersion)
            {
                CountLine = $"The {Things(ReflectionsOnly)}s could not be counted: {EngineWords.Reason(e)}.";
            }
        }
    }

    private static string Things(bool reflectionsOnly) => reflectionsOnly ? "reflection" : "consultation";

    private static string CountText(BackupPeriod period, int count, bool reflectionsOnly)
    {
        if (count == 0)
        {
            var none = reflectionsOnly ? "no reflections" : "no finished consultations";
            return period == BackupPeriod.Everything
                ? $"There are {none} to back up."
                : $"There are {none} in this period.";
        }

        var counted = Words.Count(count, Things(reflectionsOnly));
        return period == BackupPeriod.Everything ? $"{counted} on this computer." : $"{counted}, {period.Span()}.";
    }

    private async Task StartAsync()
    {
        if (!CanStart)
        {
            return;
        }

        Error = "";
        var period = Period;
        var reflectionsOnly = ReflectionsOnly;
        var name = period.FileName(Today, reflectionsOnly ? "reflections backup" : "backup");
        var path = await _picker.PickSaveAsync(name, "ClinicAVT backup", ".clinicavt")
            .ConfigureAwait(true);
        if (path is null)
        {
            return;
        }

        _folder = Path.GetDirectoryName(path) ?? "";
        OneDriveLine = BackupWords.OneDriveLine(_folder, _environment);
        SavedLine = BackupWords.SavedLine(path);
        _sentFrom = period.From(Zone);
        _sentTo = period.To(Zone);
        Progress = 0;
        ProgressText = "Starting the backup…";
        Step = BackupStep.Working;
        try
        {
            await _engine.BackUpAsync(_sentFrom, _sentTo, path, Password, reflectionsOnly)
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
                    ? $"Backing up {progress.Done} of {Words.Count(progress.Total, Things(ReflectionsOnly))}…"
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
        DoneLine = $"{Words.Count(done.Consultations, Things(done.ReflectionsOnly))} backed up and checked.";
        Step = BackupStep.Done;
        // A reflections-only file backs up no consultation, so the reminder still counts them all
        if (done.ReflectionsOnly)
        {
            return;
        }

        var createdAt = string.IsNullOrEmpty(done.CreatedAt)
            ? _clock.GetUtcNow().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture)
            : done.CreatedAt;
        _preferences.Update(p => p.LastBackup = new LastBackup(_sentFrom, _sentTo, createdAt, done.Consultations));
    }

    public void Dispose() => _engine.NotificationReceived -= OnNotification;
}
