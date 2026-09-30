using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Guidance;

/// <summary>
/// Fed from the wire by the consultation view model. It never calls the engine itself.
/// </summary>
public sealed partial class GuidanceViewModel : ObservableObject
{
    private const string NoteStaleCaption = "This guidance was found before your note edits.";

    private const string DocumentsStaleCaption =
        "Added documents changed since this guidance was found.";

    private readonly GuidanceAvailability _availability;
    private readonly ILauncher _launcher;
    private readonly IClipboard _clipboard;
    private readonly IStatusLine _status;
    private readonly IDocumentPages _pages;
    private readonly TimeProvider _time;
    // When the note's search started, null once its time is shown
    private long? _noteSearchStarted;
    private List<GuidanceRecommendation> _noteResults = [];
    private IReadOnlyList<GuidanceRecommendation>? _queryResults;
    private string _queryText = "";

    public GuidanceViewModel(
        GuidanceAvailability availability, ILauncher launcher, IClipboard clipboard, IStatusLine status,
        IDocumentPages pages, IEngineEvents events, TimeProvider time)
    {
        _availability = availability;
        _launcher = launcher;
        _clipboard = clipboard;
        _status = status;
        _pages = pages;
        _time = time;
        availability.PropertyChanged += (_, _) =>
        {
            OnPropertyChanged(nameof(Readiness));
            OnPropertyChanged(nameof(StateCaption));
            OnPropertyChanged(nameof(CaptionVisible));
            OnPropertyChanged(nameof(SettingsLinkVisible));
        };
        events.SubscribeConnection(connected =>
        {
            if (!connected)
            {
                ConnectionLost();
            }
        });
    }

    public GuidanceReadiness Readiness => _availability.Readiness;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Visible), nameof(Searching), nameof(Failed),
        nameof(NotSearched), nameof(StateCaption), nameof(CaptionVisible), nameof(SettingsLinkVisible),
        nameof(SearchAgainVisible))]
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

    /// <summary>"found in 0.4 s" for the search that put the cards on screen.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(FoundInVisible))]
    public partial string FoundIn { get; private set; } = "";

    /// <summary>The note sentence under the pointer, for the note editor to light.</summary>
    [ObservableProperty]
    public partial string Hovered { get; set; } = "";

    /// <summary>A typed query's cards while one shows, otherwise the note's.</summary>
    public ObservableCollection<GuidanceCard> Cards { get; } = [];

    public event Action? Cleared;

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

    public bool CaptionVisible => StateCaption.Length > 0 && !QueryShown;

    public bool QueryShown => _queryText.Length > 0;

    public string QueryHeader => QueryShown ? $"Search: '{_queryText}'" : "";

    // With no cards, the caption gives the search state or why the engine cannot search yet
    public string StateCaption => Section switch
    {
        GuidanceSection.Hidden or GuidanceSection.Results => "",
        GuidanceSection.NothingMatched =>
            "No guidance matched this note. Try searching for a condition or treatment.",
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
            await _pages.OpenAsync(found).ConfigureAwait(true);
        }
        else if (!await _launcher.OpenLinkAsync(found.Link).ConfigureAwait(true))
        {
            _status.Append("Could not open the link - no browser answered");
        }
    }

    [RelayCommand]
    private Task ShowInDocument(GuidanceRecommendation found) => _pages.ShowAsync(found);

    [RelayCommand]
    private Task CopyCitation(GuidanceRecommendation found) =>
        _clipboard.CopyAsync(_status, found.CitationText, "Citation");

    public void Reset()
    {
        ForgetNoteResults();
        Section = GuidanceSection.Hidden;
        ClearQuery();
        Cleared?.Invoke();
    }

    /// <summary>The note is being written. Its search follows.</summary>
    public void NoteStarted()
    {
        ForgetNoteResults();
        FoundIn = "";
        Section = GuidanceSection.FollowsNote;
        ShowCards();
    }

    /// <summary>The note arrived. Any earlier result or failure is kept.</summary>
    public void NoteReady()
    {
        if (Section == GuidanceSection.FollowsNote)
        {
            Section = GuidanceSection.Searching;
            _noteSearchStarted = _time.GetTimestamp();
        }
    }

    public void NoteFailed() => Section = GuidanceSection.Hidden;

    /// <summary>The consultation view model has accepted a search-again request.</summary>
    public void SearchStarted()
    {
        Stale = false;
        Section = GuidanceSection.Searching;
        _noteSearchStarted = _time.GetTimestamp();
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
    /// Marks the result stale on guidance/documentsChanged, unless a note edit already has.
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
        ShowFoundIn(SearchClock.FoundIn(_time, ref _noteSearchStarted));
    }

    public void ApplyFailed() => Section = GuidanceSection.Failed;

    /// <summary>Shows a typed query's cards in place of the note's. Results are null while it
    /// runs.</summary>
    public void ShowQuery(string text, IReadOnlyList<GuidanceRecommendation>? results)
    {
        _queryText = text;
        _queryResults = results;
        QueryChanged();
    }

    public void ClearQuery()
    {
        _queryText = "";
        _queryResults = null;
        FoundIn = "";
        QueryChanged();
    }

    // Cleared first, so two searches of the same length still announce the second
    public void ShowFoundIn(string foundIn)
    {
        FoundIn = "";
        FoundIn = foundIn;
    }

    /// <summary>A search running when the engine drops never returns, so the cards stay.</summary>
    private void ConnectionLost()
    {
        if (Searching)
        {
            Section = _noteResults.Count > 0
                ? GuidanceSection.Results
                : GuidanceSection.NotSearched;
        }
    }

    private void ForgetNoteResults()
    {
        _noteResults = [];
        Stale = false;
        NotStored = false;
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
        OnPropertyChanged(nameof(CaptionVisible));
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
        _pages.KeepOnlyFor(Cards);
    }
}
