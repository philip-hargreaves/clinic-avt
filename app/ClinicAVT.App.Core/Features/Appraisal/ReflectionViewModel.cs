using System.Collections.ObjectModel;
using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Appraisal;

/// <summary>
/// One appraisal reflection. The engine writes the case summary, and the clinician writes the
/// three answers and ticks the guidance they referred to. It saves only when something changed.
/// </summary>
public sealed partial class ReflectionViewModel : ObservableObject, IDisposable
{
    /// <summary>Keys of the references the clinician typed start with this.</summary>
    public const string TypedKey = "typed";

    private readonly IEngineApi _engine;
    private readonly IClipboard _clipboard;
    private readonly IFilePicker _picker;
    private readonly IDialogService _dialogs;
    private readonly StatusBarViewModel _status;
    private readonly Action<EngineNotification> _onNotification;
    private readonly Action<bool> _onConnected;
    private string _savedHappened = "";
    private string _savedLearned = "";
    private string _savedNext = "";
    private string _savedTitle = "";
    private string _savedSummary = "";
    private List<string> _savedReferences = [];
    private List<string> _savedAdded = [];

    public ReflectionViewModel(
        IEngineApi engine, IUiDispatcher dispatcher, IClipboard clipboard, IFilePicker picker,
        IDialogService dialogs, StatusBarViewModel status)
    {
        _engine = engine;
        _clipboard = clipboard;
        _picker = picker;
        _dialogs = dialogs;
        _status = status;
        _onNotification = notification => dispatcher.Post(() => HandleNotification(notification));
        _onConnected = connected => dispatcher.Post(() => HandleConnected(connected));
        _engine.NotificationReceived += _onNotification;
        _engine.ConnectedChanged += _onConnected;
        _status.PropertyChanged += OnStatusChanged;
    }

    /// <summary>The consultation's label. Typing here renames the consultation.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DisplayTitle))]
    public partial string Title { get; set; } = "";

    /// <summary>Dated to the month. The exact date never leaves the device.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DisplayTitle))]
    public partial string Month { get; private set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Warning))]
    [NotifyPropertyChangedFor(nameof(HasWarning))]
    public partial string Summary { get; set; } = "";

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(RewriteSummaryCommand))]
    [NotifyPropertyChangedFor(nameof(SummaryPlaceholder))]
    public partial bool SummaryPending { get; private set; }

    /// <summary>Said only while a summary is on its way, never over a failed one. A summary
    /// asked for during a load waits for it, and says so.</summary>
    public string SummaryPlaceholder =>
        !SummaryPending ? ""
        : _status.ModelLoading ? $"Waiting for the note model to load · {_status.ModelLoadElapsed}"
        : "Writing the case study";

    /// <summary>Why there is no summary, when the engine could not write one.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasSummaryProblem))]
    public partial string SummaryProblem { get; private set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Warning))]
    [NotifyPropertyChangedFor(nameof(HasWarning))]
    public partial string Happened { get; set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Warning))]
    [NotifyPropertyChangedFor(nameof(HasWarning))]
    public partial string Learned { get; set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Warning))]
    [NotifyPropertyChangedFor(nameof(HasWarning))]
    public partial string Next { get; set; } = "";

    /// <summary>The line being typed. Enter turns it into an entry.</summary>
    [ObservableProperty]
    public partial string Draft { get; set; } = "";

    public string SessionId { get; private set; } = "";

    /// <summary>
    /// The title as shown and exported. It is the label, or the month until there is one.
    /// </summary>
    public string DisplayTitle => Title.Trim().Length > 0 ? Title.Trim() : Month;

    public bool HasSummaryProblem => SummaryProblem.Length > 0;

    /// <summary>What in the text may identify the patient, empty when nothing was found.</summary>
    public string Warning => IdentifierCheck.Describe(Summary + "\n" + Happened + "\n" + Learned + "\n" + Next);

    public bool HasWarning => Warning.Length > 0;

    /// <summary>
    /// The guidelines and documents the consultation's guidance drew on, plus any ticked one it
    /// does not show. A tick saves.
    /// </summary>
    public ObservableCollection<ReflectionReferenceRow> References { get; } = [];

    public bool HasReferences => References.Count > 0;

    /// <summary>Guidance the clinician added themselves, one line each, always included.</summary>
    public ObservableCollection<AddedGuidanceRow> Added { get; } = [];

    public bool HasAdded => Added.Count > 0;

    public bool Dirty =>
        Happened != _savedHappened || Learned != _savedLearned || Next != _savedNext
        || !TickedIds().SequenceEqual(_savedReferences) || !AddedTitles().SequenceEqual(_savedAdded);

    public string ExportText => ReflectionExport.Format(
        new ReflectionEntry(DisplayTitle, Month, Summary, Happened, Learned, Next) { References = Ticked() });

    /// <summary>Loads the stored entry and asks for a summary when none exists yet.</summary>
    public async Task LoadAsync(string sessionId, string startedAt = "")
    {
        SessionId = sessionId;
        Month = Words.Month(Words.LocalTime(startedAt) ?? DateTimeOffset.Now);
        var opened = await EngineCall.ReportAsync(_status, "could not open the reflection", async () =>
        {
            var got = await _engine.GetReflectionAsync(sessionId).ConfigureAwait(true);
            Title = _savedTitle = got.Label ?? "";
            Summary = _savedSummary = got.Summary?.Text ?? "";
            if (got.Answers is { } answers)
            {
                Happened = _savedHappened = Answer(answers.Happened ?? "");
                Learned = _savedLearned = Answer(answers.Learned ?? "");
                Next = _savedNext = Answer(answers.Next ?? "");
            }
            else
            {
                Happened = Learned = Next = _savedHappened = _savedLearned = _savedNext = "";
            }

            await LoadReferencesAsync(got.Answers?.References ?? []).ConfigureAwait(true);
        }).ConfigureAwait(true);

        if (opened && Summary.Length == 0)
        {
            await RequestSummaryAsync().ConfigureAwait(true);
        }
    }

    /// <summary>Writes the answers and ticks only when they changed.</summary>
    public async Task SaveAsync()
    {
        // A whitespace-only answer counts as empty, because a stray line break would hide the hint
        // and count as writing
        Happened = Answer(Happened);
        Learned = Answer(Learned);
        Next = Answer(Next);
        if (!Dirty || SessionId.Length == 0)
        {
            return;
        }

        if (await EngineCall.ReportAsync(_status, "could not save the reflection",
                () => _engine.UpdateReflectionAsync(SessionId, Happened, Learned, Next, Ticked()))
            .ConfigureAwait(true))
        {
            _savedHappened = Happened;
            _savedLearned = Learned;
            _savedNext = Next;
            _savedReferences = TickedIds();
            _savedAdded = AddedTitles();
        }
    }

    /// <summary>
    /// A retitle renames the consultation itself. A blank title keeps the old name.
    /// </summary>
    public async Task SaveTitleAsync() =>
        _savedTitle = await SessionLabel.SaveAsync(_engine, _status, SessionId, Title, _savedTitle)
            .ConfigureAwait(true);

    /// <summary>Saves the clinician's correction to the summary as their own wording.</summary>
    public async Task SaveSummaryAsync()
    {
        if (SessionId.Length == 0 || Summary == _savedSummary)
        {
            return;
        }

        if (await EngineCall.ReportAsync(_status, "could not save the summary",
                () => _engine.UpdateReflectionSummaryAsync(SessionId, Summary)).ConfigureAwait(true))
        {
            _savedSummary = Summary;
        }
    }

    /// <summary>Saves everything changed as the sheet closes, then lets the engine go.</summary>
    public async Task CloseAsync()
    {
        try
        {
            await SaveAsync().ConfigureAwait(true);
            await SaveTitleAsync().ConfigureAwait(true);
        }
        finally
        {
            Dispose();
        }
    }

    public void Dispose()
    {
        _engine.NotificationReceived -= _onNotification;
        _engine.ConnectedChanged -= _onConnected;
        _status.PropertyChanged -= OnStatusChanged;
    }

    private void OnStatusChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (SummaryPending && e.PropertyName is nameof(StatusBarViewModel.ModelLoadLine))
        {
            OnPropertyChanged(nameof(SummaryPlaceholder));
        }
    }

    // One row per review card, in card order. A ticked guideline the review does not show keeps
    // its row from the stored words. No guidance at all leaves no section
    private async Task LoadReferencesAsync(IReadOnlyList<ReflectionReference> ticked)
    {
        var guidance = await EngineCall.ReportAsync(_status, "could not read the consultation's guidance",
            () => _engine.StoredGuidanceAsync(SessionId)).ConfigureAwait(true);
        Added.Clear();
        foreach (var typed in ticked.Where(r => IsTyped(r.Key)))
        {
            Added.Add(Row(typed));
        }

        _savedAdded = AddedTitles();
        OnPropertyChanged(nameof(HasAdded));
        ticked = ticked.Where(r => !IsTyped(r.Key)).ToList();
        var tickedKeys = ticked.Select(r => r.Key).ToHashSet(StringComparer.Ordinal);
        foreach (var row in References)
        {
            row.PropertyChanged -= OnReferenceChanged;
        }

        References.Clear();
        if (guidance is not null)
        {
            foreach (var card in GuidanceCard.Group(GuidanceRecommendation.ReadAll(guidance, true)))
            {
                var row = ReflectionReferenceRow.From(card, false);
                row.Ticked = tickedKeys.Contains(row.Key);
                References.Add(row);
            }
        }

        var listed = References.Select(r => r.Key).ToHashSet(StringComparer.Ordinal);
        foreach (var reference in ticked.Where(r => !listed.Contains(r.Key)))
        {
            References.Add(new ReflectionReferenceRow(reference, true));
        }

        _savedReferences = TickedIds();
        foreach (var row in References)
        {
            row.PropertyChanged += OnReferenceChanged;
        }

        OnPropertyChanged(nameof(HasReferences));
    }

    // A tick saves at once, so no view has to
    private void OnReferenceChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(ReflectionReferenceRow.Ticked))
        {
            _ = SaveAsync();
        }
    }

    private AddedGuidanceRow Row(ReflectionReference stored)
    {
        AddedGuidanceRow? row = null;
        row = new AddedGuidanceRow(stored, new AsyncRelayCommand(() => RemoveAddedAsync(row!)));
        return row;
    }

    private static bool IsTyped(string key) => key.StartsWith(TypedKey, StringComparison.Ordinal);

    private List<string> TickedIds() =>
        References.Where(r => r.Ticked).Select(r => r.Key).ToList();

    private List<string> AddedTitles() => Added.Select(r => r.Title).ToList();

    // The ticked rows, then what the clinician added, each a reference of its own
    private List<ReflectionReference> Ticked() =>
        [.. References.Where(r => r.Ticked).Select(r => r.Stored), .. Added.Select(r => r.Stored)];

    /// <summary>The typed line becomes an entry and is saved. A blank line adds nothing.</summary>
    [RelayCommand]
    private async Task AddDraft()
    {
        var text = Draft.Trim();
        Draft = "";
        if (text.Length == 0)
        {
            return;
        }

        Added.Add(Row(new ReflectionReference($"{TypedKey}:{Guid.NewGuid():N}", "", text)));
        OnPropertyChanged(nameof(HasAdded));
        await SaveAsync().ConfigureAwait(true);
    }

    private async Task RemoveAddedAsync(AddedGuidanceRow row)
    {
        if (Added.Remove(row))
        {
            OnPropertyChanged(nameof(HasAdded));
            await SaveAsync().ConfigureAwait(true);
        }
    }

    [RelayCommand(CanExecute = nameof(CanRewriteSummary))]
    private Task RewriteSummary() => RequestSummaryAsync();

    private bool CanRewriteSummary() => !SummaryPending && SessionId.Length > 0;

    private async Task RequestSummaryAsync()
    {
        SummaryPending = true;
        SummaryProblem = "";
        try
        {
            await _engine.SummariseReflectionAsync(SessionId).ConfigureAwait(true);
        }
        catch (Exception e)
        {
            _status.Log($"reflection/summary failed: {e.Message}");
            SummaryPending = false;
            SummaryProblem = $"No summary: {EngineWords.Reason(e)}. Rewrite to try again";
        }
    }

    // A summary on its way is lost with the engine that was writing it
    private void HandleConnected(bool connected)
    {
        if (!connected && SummaryPending)
        {
            SummaryPending = false;
            SummaryProblem = "No summary: ClinicAVT restarted. Rewrite to try again";
        }
    }

    [RelayCommand]
    private Task Copy() => _clipboard.CopyAsync(_status, ExportText, "Reflection");

    [RelayCommand]
    private Task SaveAsText() =>
        ReflectionFile.SaveAsync(_dialogs, _picker, _status, ExportText, DisplayTitle, Warning);

    private void HandleNotification(EngineNotification notification)
    {
        switch (notification)
        {
            case ReflectionSummaryReady ready when ready.Id == SessionId:
                Summary = _savedSummary = ready.Text;
                SummaryPending = false;
                SummaryProblem = "";
                break;
            case ReflectionSummaryFailed failed when failed.Id == SessionId:
                SummaryPending = false;
                SummaryProblem = $"No summary: {failed.Detail}";
                break;
            default:
                break;
        }
    }

    private static string Answer(string text) => text.Trim().Length == 0 ? "" : text;
}
