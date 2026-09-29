using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Guidance;

/// <summary>
/// The Guidelines section's searches: the typed query, whose cards stand in for the note's, and
/// searching the note again.
/// </summary>
public sealed partial class GuidanceSearchViewModel : ObservableObject
{
    private readonly GuidanceViewModel _guidance;
    private readonly GuidanceAvailability _availability;
    private readonly IGuidanceApi _engine;
    private readonly IGuidanceSearch _search;
    private readonly IStatusLine _status;
    private readonly TimeProvider _time;
    // When the typed query was sent, null once its time is shown
    private long? _queryStarted;
    private string _queryText = "";
    private List<GuidanceRecommendation>? _queryResults;

    public GuidanceSearchViewModel(
        GuidanceViewModel guidance, GuidanceAvailability availability, IGuidanceApi engine,
        IGuidanceSearch search, IStatusLine status, IEngineEvents events, TimeProvider time)
    {
        _guidance = guidance;
        _availability = availability;
        _engine = engine;
        _search = search;
        _status = status;
        _time = time;
        guidance.PropertyChanged += OnGuidanceChanged;
        availability.PropertyChanged += (_, _) => GatesChanged();
        guidance.Cleared += Forget;
        events.SubscribeConnection(connected =>
        {
            if (!connected)
            {
                ApplyFailed();
            }
        });
    }

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

    /// <summary>The box waits while either search runs. The button also needs a query.</summary>
    public bool QueryBoxEnabled => _availability.Readiness == GuidanceReadiness.Ready && _guidance.Visible
        && !_guidance.Searching && !QuerySearching;

    public bool SearchEnabled => QueryBoxEnabled && Query.Trim().Length > 0;

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

    [RelayCommand(CanExecute = nameof(CanSearchNote))]
    private Task SearchNote() => _search.SearchNoteAsync();

    private bool CanSearchNote() => _availability.Readiness == GuidanceReadiness.Ready
        && !_guidance.Searching
        && _guidance.Section is not (GuidanceSection.Hidden or GuidanceSection.FollowsNote);

    [RelayCommand(CanExecute = nameof(SearchEnabled))]
    private async Task SearchQuery()
    {
        var text = _queryText = Query.Trim();
        _queryResults = null;
        QueryFailed = false;
        QuerySearching = true;
        _queryStarted = _time.GetTimestamp();
        CaptionChanged();
        _guidance.ShowQuery(text, null);
        if (!await EngineCall.TryAsync(_status, "guidance/search", () => _engine.SearchGuidanceAsync(text, 3))
            .ConfigureAwait(true))
        {
            ApplyFailed();
        }
    }

    [RelayCommand(CanExecute = nameof(CanClearQuery))]
    private void ClearQuery()
    {
        Forget();
        _guidance.ClearQuery();
    }

    private bool CanClearQuery() => _guidance.QueryShown;

    // A query reply after Clear or a new consultation belongs to nothing on screen
    public void ApplyReady(GuidanceRecord result)
    {
        if (!QuerySearching)
        {
            return;
        }

        _queryResults = GuidanceRecommendation.ReadAll(result, false);
        QuerySearching = false;
        QueryFailed = false;
        _guidance.ShowFoundIn(SearchClock.FoundIn(_time, ref _queryStarted));
        CaptionChanged();
        _guidance.ShowQuery(_queryText, _queryResults);
    }

    public void ApplyFailed()
    {
        if (!QuerySearching)
        {
            return;
        }

        QuerySearching = false;
        QueryFailed = true;
    }

    private void Forget()
    {
        Query = "";
        _queryText = "";
        _queryResults = null;
        QuerySearching = false;
        QueryFailed = false;
        CaptionChanged();
    }

    private void CaptionChanged()
    {
        OnPropertyChanged(nameof(QueryCaption));
        OnPropertyChanged(nameof(QueryCaptionVisible));
    }

    private void OnGuidanceChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(GuidanceViewModel.Section):
                GatesChanged();
                break;
            case nameof(GuidanceViewModel.QueryShown):
                ClearQueryCommand.NotifyCanExecuteChanged();
                break;
        }
    }

    private void GatesChanged()
    {
        OnPropertyChanged(nameof(QueryBoxEnabled));
        OnPropertyChanged(nameof(SearchEnabled));
        SearchNoteCommand.NotifyCanExecuteChanged();
        SearchQueryCommand.NotifyCanExecuteChanged();
    }
}
