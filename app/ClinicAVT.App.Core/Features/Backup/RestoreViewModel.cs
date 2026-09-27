using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Backup;

public enum RestoreStep
{
    Choose,
    Checking,
    Summary,
    Restoring,
    Done,
}

/// <summary>
/// The Restore dialog. The engine reads the whole file first and says what it holds and what is
/// already here; only then does Restore add anything. Consultations already here are skipped.
/// </summary>
public sealed partial class RestoreViewModel : ObservableObject, IDisposable
{
    private readonly IEngineApi _engine;
    private readonly IFilePicker _picker;
    private readonly IUiDispatcher? _dispatcher;
    private readonly TimeProvider _clock;

    public RestoreViewModel(IEngineApi engine, IFilePicker picker, AppPreferences? preferences = null,
        IUiDispatcher? dispatcher = null, TimeProvider? clock = null)
    {
        _engine = engine;
        _picker = picker;
        _dispatcher = dispatcher;
        _clock = clock ?? TimeProvider.System;
        // Restoring into a computer set to keep nothing would go against that choice
        KeepingOff = preferences is { KeepConsultations: false };
        _engine.NotificationReceived += OnNotification;
    }

    public string Caption { get; } = BackupWords.RestoreCaption;

    public bool KeepingOff { get; }

    public string KeepingOffLine { get; } =
        "Turn on Save consultation data in Settings to restore consultations.";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Choosing), nameof(Busy), nameof(Summarised), nameof(Finished),
        nameof(PrimaryText), nameof(CloseText), nameof(PrimaryEnabled), nameof(BlocksClose))]
    public partial RestoreStep Step { get; private set; }

    public bool Choosing => Step == RestoreStep.Choose;

    public bool Busy => Step is RestoreStep.Checking or RestoreStep.Restoring;

    public bool Summarised => Step == RestoreStep.Summary;

    public bool Finished => Step == RestoreStep.Done;

    public string PrimaryText => Step switch
    {
        RestoreStep.Choose => "Open",
        RestoreStep.Summary when ToAdd > 0 => "Restore",
        _ => "",
    };

    public string CloseText => Step switch
    {
        RestoreStep.Checking or RestoreStep.Restoring => "",
        RestoreStep.Done => "Done",
        RestoreStep.Summary when ToAdd == 0 => "Close",
        _ => "Cancel",
    };

    public bool PrimaryEnabled => Step switch
    {
        RestoreStep.Choose => !KeepingOff && Path.Length > 0 && Password.Length > 0,
        RestoreStep.Summary => ToAdd > 0,
        _ => false,
    };

    public bool BlocksClose => Busy;

    /// <summary>True once consultations were added, so the lists reload.</summary>
    public bool RestoredAny { get; private set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(FileName), nameof(PrimaryEnabled))]
    public partial string Path { get; private set; } = "";

    public string FileName => Path.Length == 0 ? "No file chosen" : System.IO.Path.GetFileName(Path);

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PrimaryEnabled))]
    public partial string Password { get; set; } = "";

    /// <summary>What went wrong, in plain words, under the password.</summary>
    [ObservableProperty]
    public partial string Error { get; private set; } = "";

    [ObservableProperty]
    public partial string ProgressText { get; private set; } = "";

    [ObservableProperty]
    public partial double Progress { get; private set; }

    [ObservableProperty]
    public partial string SummaryLine { get; private set; } = "";

    [ObservableProperty]
    public partial string DoneLine { get; private set; } = "";

    // What the dry run found to add
    private int ToAdd { get; set; }

    [RelayCommand]
    private async Task ChooseFile()
    {
        if (await _picker.PickFileAsync(".clinicavt").ConfigureAwait(true) is { } path)
        {
            Path = path;
            Error = "";
        }
    }

    [RelayCommand]
    private Task Primary() => Step switch
    {
        RestoreStep.Choose when PrimaryEnabled => RunAsync(dryRun: true),
        RestoreStep.Summary when ToAdd > 0 => RunAsync(dryRun: false),
        _ => Task.CompletedTask,
    };

    private async Task RunAsync(bool dryRun)
    {
        Error = "";
        Progress = 0;
        ProgressText = dryRun ? "Checking the backup…" : "Restoring consultations…";
        var back = Step;
        Step = dryRun ? RestoreStep.Checking : RestoreStep.Restoring;
        try
        {
            await _engine.RestoreAsync(Path, Password, dryRun).ConfigureAwait(true);
        }
        catch (Exception e)
        {
            Error = BackupWords.Refused("restore", e);
            Step = back;
        }
    }

    private void OnNotification(EngineNotification notification) => _dispatcher.PostOrRun(() =>
    {
        if (!Busy)
        {
            return;
        }

        switch (notification)
        {
            case ArchiveProgress { Job: "restore" } progress:
                Progress = progress.Total > 0 ? (double)progress.Done / progress.Total : 0;
                if (Step == RestoreStep.Restoring)
                {
                    ProgressText = $"Restoring {progress.Done} of {Words.Count(progress.Total, "consultation")}…";
                }

                break;
            case ArchiveDone { Job: "restore" } done when Step == RestoreStep.Checking:
                Summarise(done);
                break;
            case ArchiveDone { Job: "restore" } done:
                RestoredAny = done.Consultations > 0;
                DoneLine = $"{Words.Count(done.Consultations, "consultation")} restored.";
                Step = RestoreStep.Done;
                break;
            case ArchiveFailed { Job: "restore" } failed:
                Error = BackupWords.Failure("restore", failed.Code);
                // A failure part way through a restore leaves nothing to summarise again
                Step = RestoreStep.Choose;
                break;
        }
    });

    // "33 consultations to restore, 1 Jul to 30 Sep 2026." Only what would be added counts
    private void Summarise(ArchiveDone done)
    {
        ToAdd = done.Consultations;
        if (done.Consultations + done.Skipped == 0)
        {
            SummaryLine = "This backup has no consultations.";
        }
        else if (done.Consultations == 0)
        {
            SummaryLine = "Everything in this backup is already in ClinicAVT.";
        }
        else
        {
            var span = BackupPeriod.FromWire(done.From, done.To, _clock.LocalTimeZone).Span();
            if (span.Length == 0 && Words.ShortDate(done.CreatedAt ?? "") is { Length: > 0 } made)
            {
                span = $"backed up on {made}";
            }

            var count = Words.Count(done.Consultations, "consultation");
            SummaryLine = span.Length > 0 ? $"{count} to restore, {span}." : $"{count} to restore.";
        }

        Step = RestoreStep.Summary;
    }

    public void Dispose() => _engine.NotificationReceived -= OnNotification;
}
