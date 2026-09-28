using System.Globalization;
using System.Text;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Backup;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Appraisal;

/// <summary>
/// Export reflections dialog. Writes a period's reflections to one plain text file in the Copy
/// format, without samples.
/// </summary>
public sealed partial class ExportReflectionsViewModel : ObservableObject
{
    public const string Warning = "Check each summary for identifying details before sharing.";

    private const string Rule = "----------------------------------------";

    private readonly IEngineApi _engine;
    private readonly IFilePicker _picker;
    private readonly ILauncher _launcher;
    private readonly TimeProvider _clock;
    private readonly ILogger? _logger;
    private IReadOnlyList<ReflectionListing> _all = [];
    private bool _loaded;
    private string _folder = "";

    public ExportReflectionsViewModel(IEngineApi engine, IFilePicker picker, ILauncher launcher,
        TimeProvider? clock = null, ILogger? logger = null)
    {
        _engine = engine;
        _logger = logger;
        _picker = picker;
        _launcher = launcher;
        _clock = clock ?? TimeProvider.System;
        PeriodOptions = Enumerable.Range(0, BackupPeriod.Kinds.Count)
            .Select(i => BackupPeriod.Label(i, Today)).ToArray();
    }

    public string Caption { get; } =
        "Reflections are saved as plain text, without a password, for your appraisal portfolio.";

    public string Includes { get; } =
        "Includes each case study, your answers and the guidance you referred to. Transcripts, notes "
        + "and patient information are not included.";

    /// <summary>Shown again after saving; also the file's first line.</summary>
    public string CheckLine { get; } = Warning;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(InSetup), nameof(Working), nameof(Finished), nameof(PrimaryText),
        nameof(CloseText), nameof(PrimaryEnabled), nameof(CanStart), nameof(BlocksClose))]
    public partial BackupStep Step { get; private set; }

    public bool InSetup => Step == BackupStep.Setup;

    public bool Working => Step == BackupStep.Working;

    public bool Finished => Step == BackupStep.Done;

    public string PrimaryText => Step == BackupStep.Setup ? "Choose where to save" : "";

    public string CloseText => Step switch
    {
        BackupStep.Setup => "Cancel",
        BackupStep.Working => "",
        _ => "Done",
    };

    public bool PrimaryEnabled => CanStart;

    public bool BlocksClose => Step == BackupStep.Working;

    [ObservableProperty]
    public partial string Error { get; private set; } = "";

    public IReadOnlyList<string> PeriodOptions { get; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ChoosingDates), nameof(CanStart), nameof(PrimaryEnabled))]
    public partial int PeriodIndex { get; set; } = BackupPeriod.EverythingIndex;

    partial void OnPeriodIndexChanged(int value) => Recount();

    public bool ChoosingDates => PeriodIndex == BackupPeriod.ChooseDatesIndex;

    [ObservableProperty]
    public partial DateTimeOffset? ChosenFrom { get; set; }

    partial void OnChosenFromChanged(DateTimeOffset? value) => Recount();

    [ObservableProperty]
    public partial DateTimeOffset? ChosenTo { get; set; }

    partial void OnChosenToChanged(DateTimeOffset? value) => Recount();

    public BackupPeriod Period => BackupPeriod.For(PeriodIndex, Today,
        ChosenFrom is { } from ? DateOnly.FromDateTime(from.Date) : null,
        ChosenTo is { } to ? DateOnly.FromDateTime(to.Date) : null);

    /// <summary>How many reflections the period holds, and its dates.</summary>
    [ObservableProperty]
    public partial string CountLine { get; private set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanStart), nameof(PrimaryEnabled))]
    public partial int Count { get; private set; }

    public bool CanStart => Step == BackupStep.Setup && Count > 0 && Period.Valid;

    [ObservableProperty]
    public partial string DoneLine { get; private set; } = "";

    [ObservableProperty]
    public partial string SavedLine { get; private set; } = "";

    private DateOnly Today => DateOnly.FromDateTime(_clock.GetLocalNow().DateTime);

    private TimeZoneInfo Zone => _clock.LocalTimeZone;

    /// <summary>Reads the reflections once and counts the default period.</summary>
    public async Task LoadAsync()
    {
        CountLine = "Counting reflections…";
        try
        {
            _all = (await _engine.ListReflectionsAsync().ConfigureAwait(true)).Where(r => !r.Demo).ToList();
            _loaded = true;
            Recount();
        }
        catch (Exception e)
        {
            CountLine = $"The reflections could not be counted: {EngineWords.Reason(e, _logger)}.";
        }
    }

    [RelayCommand]
    private Task Primary() => Step == BackupStep.Setup ? SaveAsync() : Task.CompletedTask;

    [RelayCommand]
    private void ShowInFolder() => _launcher.RevealFolder(_folder);

    /// <summary>The period's reflections, newest first.</summary>
    public IReadOnlyList<ReflectionListing> Chosen()
    {
        var period = Period;
        return _all
            .Select(r => (Listing: r, Started: Started(r)))
            .Where(r => r.Started is { } started && period.Holds(started, Zone))
            .OrderByDescending(r => r.Started)
            .Select(r => r.Listing)
            .ToList();
    }

    /// <summary>The file: the warning, then each reflection as Copy writes it.</summary>
    public static string Compose(IEnumerable<ReflectionEntry> entries)
    {
        var text = new StringBuilder(Warning).Append("\n\n");
        text.AppendJoin($"\n{Rule}\n\n", entries.Select(ReflectionExport.Format));
        return text.ToString();
    }

    private void Recount()
    {
        if (!_loaded)
        {
            return;
        }

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

        Count = Chosen().Count;
        var everything = period == BackupPeriod.Everything;
        var count = Words.Count(Count, "reflection");
        CountLine = Count == 0
            ? everything ? "There are no reflections to export." : "There are no reflections in this period."
            : everything ? $"{count} on this computer." : $"{count}, {period.Span()}.";
    }

    private async Task SaveAsync()
    {
        if (!CanStart)
        {
            return;
        }

        Error = "";
        var chosen = Chosen();
        var path = await _picker.PickSaveAsync(Period.FileName(Today, "reflections"), "Plain text", ".txt")
            .ConfigureAwait(true);
        if (path is null)
        {
            return;
        }

        Step = BackupStep.Working;
        var entries = new List<ReflectionEntry>();
        try
        {
            foreach (var listing in chosen)
            {
                entries.Add(await EntryAsync(listing).ConfigureAwait(true));
            }
        }
        catch (Exception e)
        {
            Error = $"The reflections could not be read: {EngineWords.Reason(e, _logger)}. Nothing was saved.";
            Step = BackupStep.Setup;
            return;
        }

        try
        {
            await File.WriteAllTextAsync(path, Compose(entries)).ConfigureAwait(true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Error = "The reflections could not be saved there. Check there is space and that you can save to "
                + "that folder, then try again.";
            Step = BackupStep.Setup;
            return;
        }

        _folder = Path.GetDirectoryName(path) ?? "";
        DoneLine = $"{Words.Count(entries.Count, "reflection")} saved.";
        SavedLine = BackupWords.SavedLine(path);
        Step = BackupStep.Done;
    }

    // Text comes from the listing; ticked guidance needs the stored reflection
    private async Task<ReflectionEntry> EntryAsync(ReflectionListing listing)
    {
        var stored = await _engine.GetReflectionAsync(listing.Id).ConfigureAwait(true);
        var label = (listing.Label ?? "").Trim();
        var month = Words.Month(Started(listing) is { } started ? TimeZoneInfo.ConvertTime(started, Zone) : default);
        return new ReflectionEntry(label.Length > 0 ? label : month, month, listing.Summary ?? "",
            listing.Happened ?? "", listing.Learned ?? "", listing.Next ?? "")
        {
            References = stored.Answers?.References ?? [],
        };
    }

    private static DateTimeOffset? Started(ReflectionListing listing) =>
        DateTimeOffset.TryParse(listing.StartedAt, CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal,
            out var parsed)
            ? parsed
            : null;
}
