using System.Collections.ObjectModel;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Appraisal;

/// <summary>The Appraisal page, a journal of reflections shown one year at a time.</summary>
public sealed partial class AppraisalsViewModel : ObservableObject, IReflectionJournal, INavigationGuard
{
    private readonly IReflectionApi _engine;
    private readonly IStatusLine _status;
    private readonly IDialogService _dialogs;
    private readonly Func<ReflectionViewModel> _editors;
    private readonly Func<ExportReflectionsViewModel> _exports;
    private readonly TimeProvider _time;
    private readonly List<ReflectionCard> _all = [];

    public AppraisalsViewModel(
        IReflectionApi engine, IStatusLine status, IDialogService dialogs,
        Func<ReflectionViewModel> editors, Func<ExportReflectionsViewModel> exports, TimeProvider time)
    {
        _engine = engine;
        _status = status;
        _dialogs = dialogs;
        _editors = editors;
        _exports = exports;
        _time = time;
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(YearLabel))]
    [NotifyCanExecuteChangedFor(nameof(PreviousYearCommand))]
    [NotifyCanExecuteChangedFor(nameof(NextYearCommand))]
    public partial int Year { get; private set; }

    [ObservableProperty]
    public partial string Search { get; set; } = "";

    [ObservableProperty]
    public partial string CountLabel { get; private set; } = "";

    /// <summary>True when there are no entries at all, so the page shows its empty state.</summary>
    [ObservableProperty]
    public partial bool Empty { get; private set; } = true;

    [ObservableProperty]
    public partial IReadOnlyList<MonthMarker> Months { get; private set; } = [];

    /// <summary>The month the list is narrowed to, 0 for the whole year.</summary>
    [ObservableProperty]
    public partial int MonthFilter { get; private set; }

    public ObservableCollection<ReflectionCard> Cards { get; } = [];

    public string YearLabel => Year == 0 ? "" : Year.ToString(CultureInfo.InvariantCulture);

    /// <summary>Pressing the year shows all of it.</summary>
    [RelayCommand]
    private void ShowWholeYear()
    {
        if (MonthFilter != 0)
        {
            MonthFilter = 0;
            Rebuild();
        }
    }

    [RelayCommand(CanExecute = nameof(CanGoToPreviousYear))]
    private async Task PreviousYear()
    {
        await CollapseAllAsync().ConfigureAwait(true);
        Year = _all.Where(c => c.Started.Year < Year).Max(c => c.Started.Year);
        MonthFilter = 0;
        Rebuild();
    }

    private bool CanGoToPreviousYear() => _all.Any(c => c.Started.Year < Year);

    [RelayCommand(CanExecute = nameof(CanGoToNextYear))]
    private async Task NextYear()
    {
        await CollapseAllAsync().ConfigureAwait(true);
        Year = _all.Where(c => c.Started.Year > Year).Min(c => c.Started.Year);
        MonthFilter = 0;
        Rebuild();
    }

    private bool CanGoToNextYear() => _all.Any(c => c.Started.Year > Year);

    /// <summary>
    /// Pressing a month narrows the year to it. Pressing it again shows the year.
    /// </summary>
    public void ToggleMonth(int month)
    {
        if (_all.All(c => c.Started.Year != Year || c.Started.Month != month))
        {
            return;
        }

        MonthFilter = MonthFilter == month ? 0 : month;
        Rebuild();
    }

    public async Task RefreshAsync()
    {
        await CollapseAllAsync().ConfigureAwait(true);
        await EngineCall.ReportAsync(_status, "could not list reflections", async () =>
        {
            var entries = await _engine.ListReflectionsAsync().ConfigureAwait(true);
            _all.Clear();
            foreach (var entry in entries)
            {
                var started = Words.LocalTime(entry.StartedAt) ?? _time.GetLocalNow();
                var label = entry.Label ?? "";
                _all.Add(new ReflectionCard(
                    this,
                    entry.Id,
                    label.Length > 0 ? label : started.ToString("d MMMM", CultureInfo.CurrentCulture),
                    started,
                    entry.Learned ?? "",
                    entry.Summary ?? "",
                    entry.Sample,
                    entry.Happened ?? "",
                    entry.Next ?? ""));
            }

            if (_all.Count == 0)
            {
                Year = _time.GetLocalNow().Year;
            }
            else if (_all.All(c => c.Started.Year != Year))
            {
                Year = _all.Max(c => c.Started.Year);
            }

            Rebuild();
        }).ConfigureAwait(true);
    }

    /// <summary>Opens one card and closes any other, which saves it.</summary>
    public async Task ToggleAsync(ReflectionCard card)
    {
        if (card.Expanded)
        {
            await CollapseAsync(card).ConfigureAwait(true);
            return;
        }

        await CollapseAllAsync().ConfigureAwait(true);
        var editor = _editors();
        await editor.LoadAsync(card.Id, card.Started.ToString("o", CultureInfo.InvariantCulture))
            .ConfigureAwait(true);
        card.Editor = editor;
        card.Expanded = true;
    }

    public Task DeleteAsync(ReflectionCard card) =>
        EngineCall.ReportAsync(_status, "could not remove the reflection", async () =>
        {
            card.Editor?.Dispose();
            card.Editor = null;
            card.Expanded = false;
            await _engine.DeleteReflectionAsync(card.Id).ConfigureAwait(true);
            _all.Remove(card);
            Rebuild();
            _status.Append("Reflection removed");
        });

    /// <summary>
    /// The Export reflections dialog. An open card saves first, so the file has its latest words.
    /// </summary>
    [RelayCommand(CanExecute = nameof(CanExport))]
    private async Task ExportReflections()
    {
        await CollapseAllAsync().ConfigureAwait(true);
        var export = _exports();
        _ = export.LoadAsync();
        await _dialogs.ShowAsync(export).ConfigureAwait(true);
    }

    // Samples are never exported, as a backup never holds them
    private bool CanExport() => _all.Any(c => !c.Sample);

    /// <summary>Leaving the page saves whatever is open.</summary>
    public Task LeaveAsync() => CollapseAllAsync();

    public Task OnNavigatingAsync(string? leaving, string arriving) =>
        leaving == Routes.Appraisals && arriving != Routes.Appraisals ? LeaveAsync() : Task.CompletedTask;

    partial void OnSearchChanged(string value) => Rebuild();

    private void Rebuild()
    {
        var needle = Search.Trim();
        var shown = _all
            .Where(c => c.Started.Year == Year)
            .Where(c => MonthFilter == 0 || c.Started.Month == MonthFilter)
            .Where(c => needle.Length == 0 || c.Matches(needle))
            .OrderByDescending(c => c.Started)
            .ToList();
        var lastMonth = -1;
        foreach (var card in shown)
        {
            card.StartsMonth = card.Started.Month != lastMonth;
            lastMonth = card.Started.Month;
        }

        Cards.Clear();
        foreach (var card in shown)
        {
            Cards.Add(card);
        }

        var inYear = _all.Count(c => c.Started.Year == Year);
        var today = _time.GetLocalNow();
        Months = Enumerable.Range(1, 12)
            .Select(m => new MonthMarker(
                m,
                CultureInfo.CurrentCulture.DateTimeFormat.GetAbbreviatedMonthName(m),
                _all.Count(c => c.Started.Year == Year && c.Started.Month == m),
                Year == today.Year && m == today.Month,
                MonthFilter == m))
            .ToList();
        CountLabel = Words.Count(inYear, "reflection");
        Empty = _all.Count == 0;
        ExportReflectionsCommand.NotifyCanExecuteChanged();
    }

    private async Task CollapseAllAsync()
    {
        foreach (var card in _all.Where(c => c.Expanded).ToList())
        {
            await CollapseAsync(card).ConfigureAwait(true);
        }

        Rebuild();
    }

    private static async Task CollapseAsync(ReflectionCard card)
    {
        if (card.Editor is { } editor)
        {
            await editor.CloseAsync().ConfigureAwait(true);
            card.Title = editor.DisplayTitle;
            card.Happened = editor.Happened;
            card.Learned = editor.Learned;
            card.Next = editor.Next;
            card.Summary = editor.CaseStudy.Summary;
        }

        card.Editor = null;
        card.Expanded = false;
    }
}
