using System.Collections.ObjectModel;
using System.Diagnostics;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Guidance;

/// <summary>
/// The Guidelines section of the review. The consultation view model owns the
/// engine and feeds this from the wire. Nothing here talks to it.
/// </summary>
public sealed partial class GuidanceViewModel : ObservableObject
{
    private const string NoteStaleCaption = "This guidance was found before your note edits.";

    private const string DocumentsStaleCaption =
        "Added documents changed since this guidance was found.";

    private readonly ILauncher _launcher;
    private readonly IClipboard _clipboard;
    private readonly StatusBarViewModel _status;
    private readonly Stopwatch _noteClock = new();
    private readonly Stopwatch _queryClock = new();
    private List<GuidanceRecommendation> _noteResults = [];
    private List<GuidanceRecommendation>? _queryResults;
    private string _queryText = "";

    public GuidanceViewModel(ILauncher launcher, IClipboard clipboard, StatusBarViewModel status)
    {
        _launcher = launcher;
        _clipboard = clipboard;
        _status = status;
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(StateCaption), nameof(CaptionVisible),
        nameof(SettingsLinkVisible),
        nameof(QueryBoxEnabled), nameof(SearchEnabled))]
    [NotifyCanExecuteChangedFor(nameof(SearchNoteCommand), nameof(SearchQueryCommand))]
    public partial GuidanceReadiness Readiness { get; private set; } = GuidanceReadiness.Loading;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Visible), nameof(Searching), nameof(Failed),
        nameof(NotSearched), nameof(StateCaption), nameof(CaptionVisible), nameof(SettingsLinkVisible),
        nameof(SearchAgainVisible), nameof(QueryBoxEnabled), nameof(SearchEnabled))]
    [NotifyCanExecuteChangedFor(nameof(SearchNoteCommand), nameof(SearchQueryCommand))]
    public partial GuidanceSection Section { get; private set; } = GuidanceSection.Hidden;

    /// <summary>The note or the added documents changed after this was found.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SearchAgainVisible))]
    public partial bool Stale { get; private set; }

    [ObservableProperty]
    public partial string StaleCaption { get; private set; } = NoteStaleCaption;

    /// <summary>
    /// The engine showed these results but could not keep them with the session.
    /// </summary>
    [ObservableProperty]
    public partial bool NotStored { get; private set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SearchEnabled))]
    [NotifyCanExecuteChangedFor(nameof(SearchQueryCommand))]
    public partial string Query { get; set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(QueryCaption), nameof(QueryCaptionVisible),
        nameof(QueryBoxEnabled), nameof(SearchEnabled))]
    [NotifyCanExecuteChangedFor(nameof(SearchQueryCommand))]
    public partial bool QuerySearching { get; private set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(QueryCaption), nameof(QueryCaptionVisible))]
    public partial bool QueryFailed { get; private set; }

    /// <summary>"found in 0.4 s" for the search that put the cards on screen.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(FoundInVisible))]
    public partial string FoundIn { get; private set; } = "";

    /// <summary>The note sentence under the pointer, for the note editor to light.</summary>
    [ObservableProperty]
    public partial string Hovered { get; set; } = "";

    /// <summary>The loader's reason when unavailable, for the log. Never shown.</summary>
    public string ReadinessDetail { get; private set; } = "";

    /// <summary>Corpora the engine refused, as "id: reason", for the log.</summary>
    public IReadOnlyList<string> RefusedCorpora { get; private set; } = [];

    /// <summary>A typed query's cards while one shows, otherwise the note's.</summary>
    public ObservableCollection<GuidanceCard> Cards { get; } = [];

    /// <summary>Set by the consultation view model, which owns the engine.</summary>
    public Func<Task>? SearchNoteRequested { get; set; }

    public Func<string, Task>? SearchQueryRequested { get; set; }

    /// <summary>The cards on screen were replaced.</summary>
    public Action? CardsShown { get; set; }

    /// <summary>A card's Show in document and Open, answered by the page view.</summary>
    public Func<GuidanceRecommendation, Task>? ShowInDocumentRequested { get; set; }

    public Func<GuidanceRecommendation, Task>? OpenDocumentRequested { get; set; }

    public bool Visible => Section != GuidanceSection.Hidden;

    public bool Searching => Section == GuidanceSection.Searching;

    public bool Failed => Section == GuidanceSection.Failed;

    public bool NotSearched => Section == GuidanceSection.NotSearched;

    /// <summary>Settings holds the guidelines folder, so the link shows when there was nothing
    /// to search as well as when guidance cannot run.</summary>
    public bool SettingsLinkVisible => Readiness == GuidanceReadiness.Unavailable
        || Section == GuidanceSection.NoCorpusAtSearch;

    public bool HasRecord => Section is GuidanceSection.Results
        or GuidanceSection.NothingMatched or GuidanceSection.NoCorpusAtSearch;

    public bool CardsVisible => Cards.Count > 0;

    public bool FoundInVisible => FoundIn.Length > 0;

    /// <summary>"2 guidelines · 3 recommendations" for the cards on screen.</summary>
    public string Summary
    {
        get
        {
            if (Cards.Count == 0)
            {
                return "";
            }

            var found = Words.Count(Cards.Sum(c => c.Recommendations.Count), "recommendation");
            var documents = Cards.Count(c => c.FromDocument);
            var guidelines = Cards.Count - documents;
            var added = Words.Count(documents, "added document");
            var installed = Words.Count(guidelines, "guideline");
            if (documents > 0 && guidelines > 0)
            {
                return $"{added} · {installed}";
            }

            return documents > 0 ? $"{added} · {found}" : $"{installed} · {found}";
        }
    }

    public bool SearchAgainVisible => Stale && !Searching;

    /// <summary>The box waits while either search runs. The button also needs a query.</summary>
    public bool QueryBoxEnabled => Readiness == GuidanceReadiness.Ready && Visible
        && !Searching && !QuerySearching;

    public bool SearchEnabled => QueryBoxEnabled && Query.Trim().Length > 0;

    public bool CaptionVisible => StateCaption.Length > 0 && !QueryShown;

    /// <summary>A typed query is on screen in place of the note's cards.</summary>
    public bool QueryShown => _queryText.Length > 0;

    public string QueryHeader => QueryShown ? $"Search: '{_queryText}'" : "";

    // What the section says when it has nothing to show. That is the search's own state, or
    // why the engine cannot search yet
    public string StateCaption => Section switch
    {
        GuidanceSection.Hidden or GuidanceSection.Results => "",
        GuidanceSection.NothingMatched => "Nothing came close enough to show. "
            + "Your documents or the installed guidance may still cover this condition.",
        GuidanceSection.NoCorpusAtSearch =>
            "No guideline documents yet. Add PDFs to the ClinicAVT guidelines folder in Documents, "
            + "then search again.",
        GuidanceSection.Failed => "Guidance could not be searched.",
        _ when Readiness != GuidanceReadiness.Ready => Readiness switch
        {
            GuidanceReadiness.Loading => "Loading the guidance model",
            _ => "Guidance is unavailable on this device.",
        },
        GuidanceSection.FollowsNote => "Guidance follows the note",
        GuidanceSection.Searching => "Finding guidance",
        _ => "Guidance was not searched for this consultation",
    };

    public string QueryCaption => QuerySearching ? "Searching"
        : QueryFailed ? "Search failed."
        : _queryResults is null ? ""
        : _queryResults.Count switch
        {
            0 => "Nothing came close enough to show.",
            1 => "1 result",
            var n => $"{n} results",
        };

    public bool QueryCaptionVisible => QueryCaption.Length > 0;

    /// <summary>
    /// Whether the note's search should run again by itself once added documents settle. That
    /// needs a result to refresh and no typed query on screen to replace.
    /// </summary>
    public bool WantsSearchAfterDocuments =>
        HasRecord && !QueryShown && !Searching && Readiness == GuidanceReadiness.Ready;

    /// <summary>A web link opens in the browser, an added document in the PDF viewer.</summary>
    [RelayCommand]
    private async Task Open(GuidanceRecommendation found)
    {
        if (found.FromDocument)
        {
            await OpenDocumentAsync(found).ConfigureAwait(true);
        }
        else if (!await _launcher.OpenLinkAsync(found.Link).ConfigureAwait(true))
        {
            _status.Append("Could not open the link - no browser answered");
        }
    }

    [RelayCommand]
    private Task ShowInDocument(GuidanceRecommendation found) =>
        ShowInDocumentRequested?.Invoke(found) ?? Task.CompletedTask;

    [RelayCommand]
    private Task CopyCitation(GuidanceRecommendation found) =>
        _clipboard.CopyAsync(_status, found.CitationText, "Citation");

    [RelayCommand(CanExecute = nameof(CanSearchNote))]
    private Task SearchNote() => SearchNoteRequested?.Invoke() ?? Task.CompletedTask;

    private bool CanSearchNote() => SearchNoteRequested is not null
        && Readiness == GuidanceReadiness.Ready && !Searching
        && Section is not (GuidanceSection.Hidden or GuidanceSection.FollowsNote);

    [RelayCommand(CanExecute = nameof(SearchEnabled))]
    private Task SearchQuery()
    {
        _queryText = Query.Trim();
        _queryResults = null;
        QueryFailed = false;
        QuerySearching = true;
        _queryClock.Restart();
        QueryChanged();
        return SearchQueryRequested?.Invoke(_queryText) ?? Task.CompletedTask;
    }

    [RelayCommand(CanExecute = nameof(QueryShown))]
    private void ClearQuery()
    {
        Query = "";
        _queryText = "";
        _queryResults = null;
        QuerySearching = false;
        QueryFailed = false;
        FoundIn = "";
        QueryChanged();
    }

    public Task OpenDocumentAsync(GuidanceRecommendation found) =>
        OpenDocumentRequested?.Invoke(found) ?? Task.CompletedTask;

    public void Reset()
    {
        ForgetNoteResults();
        Section = GuidanceSection.Hidden;
        ClearQuery();
    }

    /// <summary>The note is being written. Its search follows.</summary>
    public void NoteStarted()
    {
        ForgetNoteResults();
        FoundIn = "";
        Section = GuidanceSection.FollowsNote;
        ShowCards();
    }

    /// <summary>The note arrived. A result or failure that beat it stands.</summary>
    public void NoteReady()
    {
        if (Section == GuidanceSection.FollowsNote)
        {
            Section = GuidanceSection.Searching;
            _noteClock.Restart();
        }
    }

    public void NoteFailed() => Section = GuidanceSection.Hidden;

    /// <summary>The consultation view model has accepted a search-again request.</summary>
    public void SearchStarted()
    {
        Stale = false;
        Section = GuidanceSection.Searching;
        _noteClock.Restart();
    }

    public void MarkStale()
    {
        if (HasRecord)
        {
            StaleCaption = NoteStaleCaption;
            Stale = true;
        }
    }

    /// <summary>
    /// Marks the result stale on guidance/documentsChanged. A note edit stays the stronger reason.
    /// </summary>
    public void DocumentsChanged()
    {
        if (HasRecord && !Stale)
        {
            StaleCaption = DocumentsStaleCaption;
            Stale = true;
        }
    }

    /// <summary>
    /// A reopened session's record, or null when it was never searched. True when the
    /// added documents have changed since, so the caller can search again.
    /// </summary>
    public bool LoadStored(GuidanceRecord? guidance)
    {
        NotStored = false;
        FoundIn = "";
        if (guidance is not { } record)
        {
            _noteResults = [];
            Stale = false;
            Section = GuidanceSection.NotSearched;
            ShowCards();
            return false;
        }

        ApplyRecord(record);
        if (!record.DocumentsChanged || !HasRecord)
        {
            return false;
        }

        DocumentsChanged();
        return true;
    }

    /// <summary>The note's search came back, for the consultation on screen.</summary>
    public void ApplyReady(GuidanceRecord result)
    {
        ApplyRecord(result);
        NotStored = result.StoreError is { Length: > 0 };
        ShowFoundIn(_noteClock);
    }

    public void ApplyFailed() => Section = GuidanceSection.Failed;

    // A query reply after Clear or a new consultation belongs to nothing on screen
    public void ApplyQueryReady(GuidanceRecord result)
    {
        if (!QuerySearching)
        {
            return;
        }

        _queryResults = GuidanceRecommendation.ReadAll(result, false);
        QuerySearching = false;
        QueryFailed = false;
        ShowFoundIn(_queryClock);
        QueryChanged();
    }

    public void ApplyQueryFailed()
    {
        if (!QuerySearching)
        {
            return;
        }

        QuerySearching = false;
        QueryFailed = true;
    }

    /// <summary>
    /// Takes the embedder's state from guidance/corpora. Searching needs only that, since added
    /// documents are searched whether or not any corpus is installed.
    /// </summary>
    public void ApplyCorpora(CorporaStatus corpora)
    {
        ReadinessDetail = corpora.Detail ?? "";
        var refused = new List<string>();
        foreach (var corpus in corpora.Corpora)
        {
            var reason = corpus.Unavailable ?? "";
            if (reason.Length > 0)
            {
                refused.Add($"{corpus.Id}: {reason}");
            }
        }

        RefusedCorpora = refused;
        Readiness = corpora.State switch
        {
            "loading" => GuidanceReadiness.Loading,
            "ready" => GuidanceReadiness.Ready,
            _ => GuidanceReadiness.Unavailable,
        };
    }

    /// <summary>The poll itself failed, so there is nothing to wait for.</summary>
    public void CorporaUnavailable()
    {
        ReadinessDetail = "";
        RefusedCorpora = [];
        Readiness = GuidanceReadiness.Unavailable;
    }

    /// <summary>A search running when the engine drops never returns, so the cards stay.</summary>
    public void ConnectionLost()
    {
        if (Searching)
        {
            Section = _noteResults.Count > 0
                ? GuidanceSection.Results
                : GuidanceSection.NotSearched;
        }

        ApplyQueryFailed();
    }

    private void ForgetNoteResults()
    {
        _noteResults = [];
        Stale = false;
        NotStored = false;
    }

    // Cleared first, so two searches of the same length still announce the second
    private void ShowFoundIn(Stopwatch clock)
    {
        FoundIn = "";
        FoundIn = Elapsed(clock);
    }

    private void ApplyRecord(GuidanceRecord record)
    {
        _noteResults = GuidanceRecommendation.ReadAll(record, true);
        Section = _noteResults.Count > 0 ? GuidanceSection.Results
            : record.Searched.Count > 0 ? GuidanceSection.NothingMatched
            : GuidanceSection.NoCorpusAtSearch;
        StaleCaption = NoteStaleCaption;
        Stale = record.Stale == true;
        ShowCards();
    }

    private void QueryChanged()
    {
        OnPropertyChanged(nameof(QueryShown));
        OnPropertyChanged(nameof(QueryHeader));
        OnPropertyChanged(nameof(QueryCaption));
        OnPropertyChanged(nameof(QueryCaptionVisible));
        OnPropertyChanged(nameof(CaptionVisible));
        ClearQueryCommand.NotifyCanExecuteChanged();
        ShowCards();
    }

    // A typed query stands in for the note's cards until it is cleared. Added
    // documents lead, whole-note matches sort after the sentences, then each
    // guideline takes one card, the first after the documents under a divider
    private void ShowCards()
    {
        Cards.Clear();
        IEnumerable<GuidanceRecommendation> shown = QueryShown
            ? _queryResults ?? []
            : _noteResults.OrderBy(r => r.Trigger.Length == 0 ? 1 : 0);
        var documents = false;
        foreach (var card in GuidanceCard.Group(shown.OrderBy(r => r.FromDocument ? 0 : 1)))
        {
            if (card.FromDocument)
            {
                documents = true;
                Cards.Add(card);
            }
            else if (documents)
            {
                documents = false;
                var from = card.Labelled ? card.SourceLabel : "installed guidance";
                Cards.Add(card with { Divider = $"From {from}" });
            }
            else
            {
                Cards.Add(card);
            }
        }

        OnPropertyChanged(nameof(CardsVisible));
        OnPropertyChanged(nameof(Summary));
        CardsShown?.Invoke();
    }

    // A search the clock never timed, such as a stored record, shows nothing
    private static string Elapsed(Stopwatch clock)
    {
        if (!clock.IsRunning)
        {
            return "";
        }

        clock.Stop();
        return $"found in {clock.Elapsed.TotalSeconds:0.0} s";
    }
}
