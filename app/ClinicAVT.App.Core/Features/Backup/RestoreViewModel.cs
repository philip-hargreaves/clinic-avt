using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Extensions.Logging;
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
/// The Restore dialog. A dry run reads the whole file and counts before Restore adds anything.
/// Consultations already here are skipped.
/// </summary>
public sealed partial class RestoreViewModel : ObservableObject, IDisposable
{
    private readonly IArchiveApi _engine;
    private readonly IFilePicker _picker;
    private readonly TimeProvider _clock;
    private readonly ILogger<RestoreViewModel> _logger;
    private readonly IDisposable _notifications;
    private string _things = "consultation";

    public RestoreViewModel(IArchiveApi engine, IEngineEvents events, IFilePicker picker,
        AppPreferences preferences, TimeProvider clock, ILogger<RestoreViewModel> logger)
    {
        _engine = engine;
        _logger = logger;
        _picker = picker;
        _clock = clock;
        // Restoring into a computer set to keep nothing would go against that choice
        KeepingOff = !preferences.KeepConsultations;
        _notifications = events.Subscribe<EngineNotification>(OnNotification);
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

    private int ToAdd { get; set; }

    [RelayCommand]
    private async Task ChooseFile()
    {
        if (await _picker.PickFileAsync([".clinicavt"]).ConfigureAwait(true) is { } path)
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
        ProgressText = dryRun ? "Checking the backup…" : $"Restoring {_things}s…";
        var back = Step;
        Step = dryRun ? RestoreStep.Checking : RestoreStep.Restoring;
        try
        {
            await _engine.RestoreAsync(Path, Password, dryRun).ConfigureAwait(true);
        }
        catch (Exception e)
        {
            Error = BackupWords.Refused(ArchiveJob.Restore, e, _logger);
            Step = back;
        }
    }

    private void OnNotification(EngineNotification notification)
    {
        if (!Busy)
        {
            return;
        }

        switch (notification)
        {
            case ArchiveProgress { Job: ArchiveJob.Restore } progress:
                Progress = progress.Total > 0 ? (double)progress.Done / progress.Total : 0;
                if (Step == RestoreStep.Restoring)
                {
                    ProgressText = $"Restoring {progress.Done} of {Words.Count(progress.Total, _things)}…";
                }

                break;
            case ArchiveDone { Job: ArchiveJob.Restore } done when Step == RestoreStep.Checking:
                Summarise(done);
                break;
            case ArchiveDone { Job: ArchiveJob.Restore } done:
                RestoredAny = done.Consultations > 0;
                DoneLine = $"{Words.Count(done.Consultations, _things)} restored.";
                Step = RestoreStep.Done;
                break;
            case ArchiveFailed { Job: ArchiveJob.Restore } failed:
                Error = BackupWords.Failure(ArchiveJob.Restore, failed.Code);
                // A failure part way through a restore leaves nothing to summarise again
                Step = RestoreStep.Choose;
                break;
        }
    }

    // "33 consultations to restore, 1 Jul to 30 Sep 2026." Only what would be added counts
    private void Summarise(ArchiveDone done)
    {
        // A reflections-only backup restores each reflection with a removed consultation, so the
        // counts are reflections
        _things = done.ReflectionsOnly ? "reflection" : "consultation";
        ToAdd = done.Consultations;
        if (done.Consultations + done.Skipped == 0)
        {
            SummaryLine = $"This backup has no {_things}s.";
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

            var count = Words.Count(done.Consultations, _things);
            SummaryLine = span.Length > 0 ? $"{count} to restore, {span}." : $"{count} to restore.";
        }

        Step = RestoreStep.Summary;
    }

    public void Dispose() => _notifications.Dispose();
}
