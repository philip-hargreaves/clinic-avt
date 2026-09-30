using System.Text.Json;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;
using static ClinicAVT.App.Tests.Support.GuidanceRecords;
using static ClinicAVT.App.Tests.Support.Waits;

namespace ClinicAVT.App.Tests.Features.Guidance;

/// <summary>
/// Driven through the consultation view model.
/// </summary>
public class GuidanceViewModelTest
{
    private static readonly object Corpus = GuidanceRecords.Corpus("NICE", attribution: "Fixture attribution");

    private static readonly object DocumentCorpus = GuidanceRecords.DocumentCorpus();

    private static IEnumerable<string> Shown(GuidanceViewModel guidance) =>
        guidance.Cards.SelectMany(c => c.Recommendations).Select(r => r.ChunkId);

    private static JsonElement Note(string text) =>
        JsonSerializer.SerializeToElement(new { text });

    private static readonly JsonElement DocumentsChanged = JsonSerializer.SerializeToElement(new { });

    // Stops a consultation and lets the note arrive, so the section is searching for "s1"
    private static async Task<TestShell> AfterNoteAsync(ListLogger? log = null)
    {
        var shell = TestSession.Create(log: log);
        var (session, engine, _) = shell;
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready", Note("the clinical note"));
        return shell;
    }

    // Reopens stored consultation "abc". A null record means its note was never searched
    private static async Task<TestShell> ReopenedAsync(JsonElement? record = null, ListLogger? log = null)
    {
        var shell = TestSession.Create(log: log);
        var (session, engine, _) = shell;
        engine.StoredNote = "the stored note";
        engine.StoredGuidance = record;
        await session.OpenStoredSessionAsync("abc");
        return shell;
    }

    [Fact]
    public async Task AReopenedRecordShowsFreshGoesStaleOnlyWhenTheNoteChangesAndClearsWithTheConsultation()
    {
        var shell = await ReopenedAsync(Record([Result("fx100-1_1_1"), Result("fx100-1_1_2")]));
        var (session, engine, note) = shell;

        var guidance = shell.Guidance;
        Assert.Equal(GuidanceSection.Results, guidance.Section);
        Assert.Equal(2, guidance.Cards.Single().Recommendations.Count);
        Assert.Equal("", guidance.FoundIn);
        Assert.False(guidance.Stale);
        Assert.True(guidance.HasRecord);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "guidance/search");

        await shell.Get<DocumentActions>().SaveNoteAsync();
        Assert.False(guidance.Stale);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "note/update");

        note.ClinicalNoteText = "edited";
        await shell.Get<DocumentActions>().SaveNoteAsync();
        Assert.True(guidance.Stale);
        Assert.Contains(engine.Requests, r => r.Method == "note/update");

        await session.CloseReviewAsync();
        Assert.Equal(GuidanceSection.Hidden, guidance.Section);
        Assert.Empty(guidance.Cards);
        Assert.False(guidance.Stale);
    }

    // Note-edit caption beats documents-changed, whether stale on load or later
    [Fact]
    public async Task ChangedDocumentsMarkAStoredResultStaleWithTheirOwnCaptionUntilTheNoteIsEditedAndSearchAgainByThemselves()
    {
        var shell = await ReopenedAsync(Record([Result("fx100-1_1_1")]));
        var (session, engine, note) = shell;

        engine.RaiseNotification("guidance/documentsChanged", DocumentsChanged);

        Assert.True(shell.Guidance.Stale);
        Assert.True(shell.Guidance.SearchAgainVisible);
        Assert.Equal(
            "Added documents changed since this guidance was found.",
            shell.Guidance.StaleCaption);

        note.ClinicalNoteText = "edited";
        await shell.Get<DocumentActions>().SaveNoteAsync();

        Assert.Equal(
            "This guidance was found before your note edits.", shell.Guidance.StaleCaption);

        await session.CloseReviewAsync();
        engine.StoredGuidance = Record([Result("fx100-1_1_1")], stale: true, documentsChanged: true);
        await session.OpenStoredSessionAsync("abc");

        Assert.True(shell.Guidance.Stale);
        Assert.True(shell.Guidance.SearchAgainVisible);
        Assert.Equal(
            "This guidance was found before your note edits.", shell.Guidance.StaleCaption);

        // A note older than the documents loads stale and searches again by itself
        await session.CloseReviewAsync();
        engine.StoredGuidance = Record([], documentsChanged: true);
        shell.Get<ReviewGuidanceSearch>().DocumentsSettle = TimeSpan.FromMilliseconds(80);
        var searches = engine.Requests.Count(r => r.Method == "guidance/search");
        await session.OpenStoredSessionAsync("abc");

        Assert.True(shell.Guidance.Stale);
        Assert.Equal(
            "Added documents changed since this guidance was found.",
            shell.Guidance.StaleCaption);
        shell.Clock.Advance(TimeSpan.FromMilliseconds(80));
        await WaitUntilAsync(() => engine.Requests.Count(r => r.Method == "guidance/search") > searches);
    }

    [Fact]
    public async Task AnEmptyNoteHidesTheSectionANoteNeverSearchedSaysSoAndSoDoesAQueryWithNoMatch()
    {
        var shell = TestSession.Create();
        var (session, engine, _) = shell;
        engine.StoredGuidance = Record([Result("fx100-1_1_1")]);
        await session.OpenStoredSessionAsync("abc");
        Assert.Equal(GuidanceSection.Hidden, shell.Guidance.Section);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/guidance");

        await session.CloseReviewAsync();
        engine.StoredNote = "the stored note";
        engine.StoredGuidance = null;
        await session.OpenStoredSessionAsync("abc");

        Assert.Equal(GuidanceSection.NotSearched, shell.Guidance.Section);
        Assert.Equal(
            "Guidance was not searched for this consultation", shell.Guidance.StateCaption);
        Assert.True(shell.Search.SearchNoteCommand.CanExecute(null));
        Assert.DoesNotContain(engine.Requests, r => r.Method == "guidance/search");

        shell.Search.Query = "nothing here";
        await shell.Search.SearchQueryCommand.ExecuteAsync(null);
        engine.RaiseNotification("guidance/ready", Ready(null, [], stale: null));

        Assert.Equal("No guidance found for 'nothing here'.", shell.Search.QueryCaption);
        Assert.False(shell.Search.QuerySearching);
        Assert.Empty(shell.Guidance.Cards);
        Assert.Equal("", shell.Guidance.Summary);
        Assert.False(shell.Guidance.CardsVisible);
        Assert.Equal(GuidanceSection.NotSearched, shell.Guidance.Section);
        Assert.False(shell.Guidance.CaptionVisible, "the query bar replaces the note caption");

        shell.Search.ClearQueryCommand.Execute(null);
        Assert.True(shell.Guidance.CaptionVisible);
    }

    [Fact]
    public async Task ATypedQueryFailsInTheQueryBarWhetherTheEngineRefusesOrGoesAway()
    {
        var log = new ListLogger();
        var shell = await ReopenedAsync(log: log);
        var (session, engine, note) = shell;
        shell.Search.Query = "gout";
        await shell.Search.SearchQueryCommand.ExecuteAsync(null);

        engine.RaiseNotification("guidance/failed", Failed(null, "embedder gone"));

        Assert.Equal("Search failed.", shell.Search.QueryCaption);
        Assert.Contains(
            log.Lines, e => e.Contains("embedder gone", StringComparison.Ordinal));
        Assert.Equal(GuidanceSection.NotSearched, shell.Guidance.Section);

        await shell.Search.SearchQueryCommand.ExecuteAsync(null);
        Assert.True(shell.Search.QuerySearching);
        engine.SetConnected(false);

        Assert.False(shell.Search.QuerySearching);
        Assert.Equal("Search failed.", shell.Search.QueryCaption);
        Assert.True(shell.Guidance.QueryShown);
    }

    [Fact]
    public async Task TheSharedFixturesLoad()
    {
        var shell = await ReopenedAsync(Fixtures.Load("session-guidance.json")
            .GetProperty("result").GetProperty("guidance"));
        var (session, _, _) = shell;
        var guidance = shell.Guidance;

        var card = guidance.Cards.Single();
        Assert.Equal(
            "Fictional inflammatory joint disease: assessment and management", card.Title);
        Assert.Equal("Updated 12 Oct 2020 · 1 recommendation", card.Meta);
        Assert.Equal("NICE · FX100", card.Chip);
        Assert.Equal("nice", card.Source);
        var found = card.Recommendations.Single();
        Assert.Equal("FX100 1.1.1", found.Reference);
        Assert.Equal("[2009, amended 2018]", found.Tag);
        Assert.Equal(
            "1.1 Referral, diagnosis and investigations › Referral from primary care", found.Path);
        Assert.True(found.CanOpen);

        guidance.ApplyReady(Fixtures.Load("guidance-ready.json").GetProperty("params"));
        Assert.Equal(GuidanceSection.Results, guidance.Section);
        var cards = guidance.Cards;
        Assert.Equal(["NICE · FX100", "Fixture guidance corpus"], cards.Select(c => c.Chip));
        Assert.Equal("Matched: the note as a whole", cards[^1].Recommendations.Single().Matched);
        Assert.False(cards[1].Recommendations.Single().CanOpen);

        shell.Get<GuidanceAvailability>().ApplyCorpora(Protocol.Parse<CorporaStatus>(Fixtures.Load("guidance-corpora.json").GetProperty("result"))!);
        Assert.Equal(GuidanceReadiness.Ready, guidance.Readiness);
        Assert.Equal(
            "nice-2026-08-25: corpus.db sha256 does not match the manifest",
            shell.Get<GuidanceAvailability>().RefusedCorpora.Single());
    }

    [Fact]
    public async Task AnAddedDocumentLeadsWithItsChipAndShowInDocumentOpensThePageView()
    {
        var shell = await AfterNoteAsync();
        var (session, engine, _) = shell;
        shell.Guidance.ApplyReady(Ready("s1", [Result("fx100-1_1_1"), DocumentResult()],
            corpora: [Corpus, DocumentCorpus]));

        var cards = shell.Guidance.Cards;
        Assert.Equal(2, cards.Count);
        Assert.True(cards[0].FromDocument);
        Assert.Equal("Added document", cards[0].Chip);
        Assert.Equal("BSR PMR guidelines 2009", cards[0].Title);
        Assert.Equal("5 pages · added 15 Sep 2026 · 1 recommendation", cards[0].Meta);
        Assert.False(cards[0].DividerVisible);
        Assert.Equal("From NICE", cards[1].Divider);
        Assert.Equal("1 added document · 1 guideline", shell.Guidance.Summary);

        var found = cards[0].Recommendations.Single();
        Assert.Equal(6368831970660585267L, found.Document);
        Assert.Equal("Page 2", found.PageLabel);
        Assert.Equal("1.2", found.Reference);
        Assert.True(found.ShowVisible);
        Assert.True(found.CanOpen);
        Assert.Equal("Open BSR PMR guidelines 2009", found.OpenName);
        Assert.Equal(
            "BSR PMR guidelines 2009, page 2, 1.2 (added 15 Sep 2026)", found.CitationText);
        Assert.False(cards[1].Recommendations.Single().ShowVisible);

        engine.PageReply = new
        {
            path = @"C:\scratch\page.bmp",
            width = 1000,
            height = 1400,
            pages = 5,
            boxes = new[] { new { page = 1, left = 0.1, top = 0.2, right = 0.6, bottom = 0.3 } },
        };
        await shell.Guidance.ShowInDocumentCommand.ExecuteAsync(found);

        Assert.True(shell.Get<PageViewModel>().Visible);
        Assert.Equal("Page 2 of 5", shell.Get<PageViewModel>().PageLabel);
        Assert.Contains("\"id\":6368831970660585267", engine.Requests[^1].Params);
        Assert.Single(shell.Get<PageViewModel>().Boxes);

        shell.Guidance.Reset();
        shell.Get<PageViewModel>().Hide();
        Assert.False(shell.Get<PageViewModel>().Visible);
    }

    [Fact]
    public async Task TheNotesSearchStartsWithTheNoteTheBoxWaitsAndAnotherSessionsResultIsIgnored()
    {
        var log = new ListLogger();
        var shell = TestSession.Create(log: log);
        var (session, engine, _) = shell;
        shell.Search.Query = "gout";
        Assert.False(shell.Guidance.Visible);
        Assert.False(shell.Search.QueryBoxEnabled);
        Assert.False(shell.Search.SearchQueryCommand.CanExecute(null));

        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        Assert.Equal(GuidanceSection.FollowsNote, shell.Guidance.Section);

        engine.RaiseNotification("note/ready", Note("the clinical note"));
        Assert.Equal(GuidanceSection.Searching, shell.Guidance.Section);
        Assert.False(shell.Search.SearchEnabled);
        Assert.False(shell.Search.QueryBoxEnabled);

        engine.RaiseNotification("guidance/ready", Ready("other", [Result("fx100-1_1_1")]));
        Assert.Equal(GuidanceSection.Searching, shell.Guidance.Section);
        Assert.Empty(shell.Guidance.Cards);
        Assert.Contains(
            log.Lines, e => e.Contains("other", StringComparison.Ordinal));

        engine.RaiseNotification("guidance/ready", Ready("s1", [Result("fx100-1_1_1")]));
        Assert.Equal(GuidanceSection.Results, shell.Guidance.Section);
        Assert.False(shell.Guidance.Stale);
    }

    // Sentence matches lead whole-note matches, both across cards and within one
    [Fact]
    public async Task ResultsAreOrderedAndEmptyUnknownCorpusAndUnstoredRepliesEachReadTheirOwnWay()
    {
        var log = new ListLogger();
        var shell = await AfterNoteAsync(log);
        var (session, engine, _) = shell;

        engine.RaiseNotification("guidance/ready", Ready("s1",
        [
            Result("fx200-1_1_1", trigger: ""),
            Result("fx100-1_1_1", trigger: "Second sentence."),
            Result("fx200-1_1_2", trigger: "Third sentence."),
        ]));

        Assert.Equal(["FX100", "FX200"], shell.Guidance.Cards.Select(c => c.Code));
        Assert.Equal(["fx100-1_1_1", "fx200-1_1_2", "fx200-1_1_1"], Shown(shell.Guidance));
        Assert.Equal("2 guidelines · 3 recommendations", shell.Guidance.Summary);
        Assert.True(shell.Guidance.CardsVisible);
        Assert.Matches(@"^found in \d+\.\d s$", shell.Guidance.FoundIn);

        engine.RaiseNotification("guidance/ready", Ready("s1", []));
        Assert.Equal(GuidanceSection.NothingMatched, shell.Guidance.Section);
        Assert.Equal(
            "No guidance matched this note. Try searching for a condition or treatment.",
            shell.Guidance.StateCaption);

        engine.RaiseNotification("guidance/ready", Ready("s1", [], searched: false));
        Assert.Equal(GuidanceSection.NoCorpusAtSearch, shell.Guidance.Section);
        Assert.StartsWith("No guideline documents yet", shell.Guidance.StateCaption);
        Assert.True(shell.Guidance.SettingsLinkVisible, "Settings holds the folder");

        // A result whose corpus was not in the searched list shows its code
        engine.RaiseNotification("guidance/ready",
            Ready("s1", [Result("gout-1", source: "text")], searched: false));
        Assert.Equal(GuidanceSection.Results, shell.Guidance.Section);
        Assert.Equal("GOUT", shell.Guidance.Cards.Single().Chip);

        engine.RaiseNotification("guidance/ready",
            Ready("s1", [Result("fx100-1_1_1")], storeError: "disk full"));
        Assert.Equal(GuidanceSection.Results, shell.Guidance.Section);
        Assert.True(shell.Guidance.NotStored);
        Assert.Contains(
            log.Lines, e => e.Contains("disk full", StringComparison.Ordinal));
    }

    [Theory]
    [InlineData("guidance/ready", GuidanceSection.Results)]
    [InlineData("guidance/failed", GuidanceSection.Failed)]
    public async Task AnOutcomeThatArrivesBeforeTheNoteStands(string method, GuidanceSection section)
    {
        var shell = TestSession.Create();
        var (session, engine, _) = shell;
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();

        engine.RaiseNotification(method,
            method == "guidance/ready" ? Ready("s1", [Result("fx100-1_1_1")]) : Failed("s1", "embedder gone"));
        engine.RaiseNotification("note/ready", Note("the clinical note"));

        Assert.Equal(section, shell.Guidance.Section);
    }

    [Fact]
    public async Task SearchAgainSavesAChangedNoteThenSearchesAndASupersededFailureChangesNothing()
    {
        var shell = await ReopenedAsync(Record([Result("fx100-1_1_1")]));
        var (session, engine, note) = shell;
        note.ClinicalNoteText = "edited";
        engine.BeforeReply = method =>
        {
            if (method == "guidance/search")
            {
                engine.RaiseNotification("guidance/failed", Failed("abc", "superseded"));
            }
        };

        await shell.Search.SearchNoteCommand.ExecuteAsync(null);

        var methods = engine.Requests.Select(r => r.Method).ToList();
        var save = methods.IndexOf("note/update");
        Assert.True(save >= 0 && save < methods.LastIndexOf("guidance/search"));
        Assert.Contains(engine.Requests,
            r => r.Method == "guidance/search"
                && r.Params.Contains("\"id\":\"abc\"", StringComparison.Ordinal));
        Assert.Equal(GuidanceSection.Searching, shell.Guidance.Section);
        Assert.False(shell.Guidance.Stale, "a search in flight is not stale");

        engine.RaiseNotification("guidance/ready", Ready("abc", [Result("fx100-1_1_2")]));
        Assert.Equal(GuidanceSection.Results, shell.Guidance.Section);
    }

    [Fact]
    public async Task AnyOtherFailureShowsTheFailedStateAndKeepsTheDetailOffScreen()
    {
        var log = new ListLogger();
        var shell = await AfterNoteAsync(log);
        var (session, engine, _) = shell;

        engine.RaiseNotification("guidance/failed",
            Failed("s1", "guidance embedder gte-large-int8: tokenizer ignores max_length"));

        Assert.Equal(GuidanceSection.Failed, shell.Guidance.Section);
        Assert.Equal("Guidance could not be searched.", shell.Guidance.StateCaption);
        Assert.Contains(
            log.Lines, e => e.Contains("tokenizer", StringComparison.Ordinal));
        Assert.DoesNotContain("tokenizer", shell.Line.LatestActivity, StringComparison.Ordinal);
        Assert.True(shell.Search.SearchNoteCommand.CanExecute(null));
    }

    [Fact]
    public async Task LosingTheEngineEndsTheSearchAndReconnectingPollsAgain()
    {
        var shell = await AfterNoteAsync();
        var (session, engine, _) = shell;
        var polls = engine.Requests.Count(r => r.Method == "guidance/corpora");

        engine.SetConnected(false);
        Assert.Equal(GuidanceSection.NotSearched, shell.Guidance.Section);

        engine.SetConnected(true);
        Assert.Equal(polls + 1, engine.Requests.Count(r => r.Method == "guidance/corpora"));
    }

    [Fact]
    public async Task ABurstOfDocumentChangesSearchesTheNoteOnceAfterTheySettle()
    {
        var shell = await ReopenedAsync(Record([Result("fx100-1_1_1")]));
        var (session, engine, note) = shell;
        shell.Get<ReviewGuidanceSearch>().DocumentsSettle = TimeSpan.FromMilliseconds(80);
        var before = engine.Requests.Count(r => r.Method == "guidance/search");

        // Thirty documents finishing in quick succession
        for (var i = 0; i < 30; i++)
        {
            engine.RaiseNotification("guidance/documentsChanged", DocumentsChanged);
        }

        Assert.True(shell.Guidance.Stale);
        shell.Clock.Advance(TimeSpan.FromMilliseconds(80));
        await WaitUntilAsync(() => engine.Requests.Count(r => r.Method == "guidance/search") != before);

        await Task.Delay(300);  // long enough for any second search to have fired
        Assert.Equal(before + 1, engine.Requests.Count(r => r.Method == "guidance/search"));
    }

    [Fact]
    public async Task DocumentChangesLeaveATypedQueryAlone()
    {
        var shell = await ReopenedAsync(Record([Result("fx100-1_1_1")]));
        var (session, engine, note) = shell;
        shell.Get<ReviewGuidanceSearch>().DocumentsSettle = TimeSpan.FromMilliseconds(50);
        shell.Search.Query = "allopurinol";
        await shell.Search.SearchQueryCommand.ExecuteAsync(null);
        var before = engine.Requests.Count(r => r.Method == "guidance/search");

        engine.RaiseNotification("guidance/documentsChanged", DocumentsChanged);
        shell.Clock.Advance(TimeSpan.FromMilliseconds(50));
        await Task.Delay(300);

        Assert.Equal(before, engine.Requests.Count(r => r.Method == "guidance/search"));
    }

    [Fact]
    public void EverySearchAnnouncesItsTimingEvenWhenTheTextRepeats()
    {
        var guidance = new TestShell().Guidance;
        var announced = 0;
        guidance.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(GuidanceViewModel.FoundIn) && guidance.FoundIn.Length > 0)
            {
                announced++;
            }
        };

        guidance.SearchStarted();
        guidance.ApplyReady(Ready("abc", [Result("fx100-1_1_2")]));
        guidance.SearchStarted();
        guidance.ApplyReady(Ready("abc", [Result("fx100-1_1_2")]));

        Assert.Equal(2, announced);
        Assert.Equal("found in 0.0 s", guidance.FoundIn);
    }

    [Fact]
    public async Task RegenerateSearchesAgainAndTheResultClearsStale()
    {
        var shell = await ReopenedAsync(Record([Result("fx100-1_1_1")], stale: true));
        var (session, engine, note) = shell;

        await shell.Commands.RegenerateCommand.ExecuteAsync(null);
        Assert.Equal(GuidanceSection.FollowsNote, shell.Guidance.Section);
        Assert.False(shell.Guidance.Stale);
        Assert.Empty(shell.Guidance.Cards);

        engine.RaiseNotification("note/ready", Note("rewritten"));
        engine.RaiseNotification("guidance/ready", Ready("abc", [Result("fx100-1_1_2")]));
        Assert.Equal(GuidanceSection.Results, shell.Guidance.Section);
        Assert.False(shell.Guidance.Stale);
    }

    [Fact]
    public async Task OpeningAnotherConsultationDuringTheSaveLeavesItAlone()
    {
        var shell = await ReopenedAsync(Record([Result("fx100-1_1_1")]));
        var (session, engine, note) = shell;
        note.ClinicalNoteText = "edited";
        var opened = false;  // the open autosaves too, so the hook fires once
        engine.BeforeReply = method =>
        {
            if (method == "note/update" && !opened)
            {
                opened = true;
                _ = session.OpenStoredSessionAsync("other");
            }
        };

        await shell.Search.SearchNoteCommand.ExecuteAsync(null);

        Assert.DoesNotContain(engine.Requests, r => r.Method == "guidance/search");
        Assert.Equal(GuidanceSection.Results, shell.Guidance.Section);
        Assert.False(shell.Guidance.Stale);
        Assert.DoesNotContain(
            "Note saved", shell.Line.LatestActivity, StringComparison.Ordinal);
    }

    [Fact]
    public async Task ATypedQueryStandsInForTheNotesCardsUntilCleared()
    {
        var shell = await ReopenedAsync(Record([Result("fx100-1_1_1")]));
        var (session, engine, note) = shell;
        var guidance = shell.Guidance;

        shell.Search.Query = "gout flare";
        Assert.True(shell.Search.SearchEnabled);
        await shell.Search.SearchQueryCommand.ExecuteAsync(null);
        Assert.True(shell.Search.QuerySearching);
        Assert.False(shell.Search.SearchEnabled);
        Assert.False(shell.Search.QueryBoxEnabled);
        Assert.Contains(engine.Requests,
            r => r.Method == "guidance/search"
                && r.Params.Contains("\"text\":\"gout flare\"", StringComparison.Ordinal));

        Assert.True(guidance.QueryShown);
        Assert.Equal("Search: 'gout flare'", guidance.QueryHeader);
        Assert.Empty(guidance.Cards);

        engine.RaiseNotification("guidance/ready",
            Ready(null, [Result("fx200-1_1_1", trigger: ""), Result("fx200-1_1_2", trigger: "")],
                stale: null));
        Assert.Equal("2 results", shell.Search.QueryCaption);
        Assert.Matches(@"^found in \d+\.\d s$", guidance.FoundIn);
        Assert.Equal(["fx200-1_1_1", "fx200-1_1_2"], Shown(guidance));
        Assert.All(guidance.Cards.Single().Recommendations, r => Assert.False(r.MatchedVisible));
        Assert.Equal(GuidanceSection.Results, guidance.Section);

        // The note's result, arriving during a typed query, shows only after Clear
        engine.RaiseNotification("guidance/ready",
            Ready("abc", [Result("fx100-1_1_2"), Result("fx100-1_1_3")]));
        Assert.Equal(["fx200-1_1_1", "fx200-1_1_2"], Shown(guidance));

        shell.Search.ClearQueryCommand.Execute(null);
        Assert.False(guidance.QueryShown);
        Assert.Equal("", shell.Search.Query);
        Assert.Equal("", guidance.FoundIn);
        Assert.Equal(["fx100-1_1_2", "fx100-1_1_3"], Shown(guidance));
    }

    [Fact]
    public async Task AQueryReplyAfterClearOrANewConsultationIsIgnored()
    {
        var shell = await ReopenedAsync(Record([Result("fx100-1_1_1")]));
        var (session, engine, note) = shell;
        shell.Search.Query = "gout";
        await shell.Search.SearchQueryCommand.ExecuteAsync(null);
        shell.Search.ClearQueryCommand.Execute(null);

        engine.RaiseNotification("guidance/ready", Ready(null, [], stale: null));
        Assert.Equal("", shell.Search.QueryCaption);
        Assert.False(shell.Guidance.QueryShown);

        shell.Search.Query = "gout";
        await shell.Search.SearchQueryCommand.ExecuteAsync(null);
        await session.CloseReviewAsync();
        engine.RaiseNotification("guidance/failed", Failed(null, "late"));
        Assert.Equal("", shell.Search.QueryCaption);
        Assert.Empty(shell.Guidance.Cards);
    }

    // Added documents can be searched without an installed corpus, so a refused one never blocks
    [Fact]
    public void ReadinessComesFromThePollThenTheNotificationAndARefusedCorpusStillSearches()
    {
        var engine = new FakeEngineClient(autoNotify: false) { GuidanceState = "loading" };
        engine.GuidanceCorpora.Clear();
        engine.GuidanceCorpora.Add(new { id = "nice", unavailable = "sha256 differs" });
        var shell = TestSession.Create(engine: engine);
        var (session, _, _) = shell;
        Assert.Equal(GuidanceReadiness.Loading, shell.Guidance.Readiness);

        engine.GuidanceState = "ready";
        engine.RaiseNotification("guidance/model",
            JsonSerializer.SerializeToElement(new { state = "ready", detail = (string?)null }));

        Assert.Equal(GuidanceReadiness.Ready, shell.Guidance.Readiness);
        Assert.Equal(2, engine.Requests.Count(r => r.Method == "guidance/corpora"));
        Assert.False(shell.Guidance.SettingsLinkVisible);
        Assert.Equal(["nice: sha256 differs"], shell.Get<GuidanceAvailability>().RefusedCorpora);
    }

    [Fact]
    public void AFailedPollOrAnUnavailableEmbedderReadsAsUnavailableAndOnlyTheLogSaysWhy()
    {
        var log = new ListLogger();
        var failing = new FakeEngineClient(autoNotify: false)
        {
            FailNext = m => m == "guidance/corpora" ? new IOException("pipe closed") : null,
        };
        var polled = TestSession.Create(engine: failing, log: log);
        Assert.Equal(GuidanceReadiness.Unavailable, polled.Guidance.Readiness);
        Assert.Contains(log.Lines, e => e.Contains("pipe closed", StringComparison.Ordinal));

        var engine = new FakeEngineClient(autoNotify: false)
        {
            GuidanceState = "unavailable",
            GuidanceDetail = "no model for embedding/default",
        };
        var shell = TestSession.Create(engine: engine, log: log);
        var status = shell.Line;

        Assert.Equal(GuidanceReadiness.Unavailable, shell.Guidance.Readiness);
        Assert.Contains(
            log.Lines, e => e.Contains("embedding/default", StringComparison.Ordinal));
        Assert.DoesNotContain("embedding", status.LatestActivity, StringComparison.Ordinal);
        Assert.DoesNotContain("embedding", shell.Guidance.StateCaption, StringComparison.Ordinal);
        shell.Search.Query = "gout";
        Assert.False(shell.Search.SearchEnabled);
    }

    [Fact]
    public void RecommendationsDisplayAsTheGuidelineWritesAndCardsGroupThemInOrder()
    {
        var found = Found(Result("fx100-1_1_1"));
        Assert.Equal("1.1 Referral", found.Path);
        Assert.Equal("Matched: “A sentence of the note.”", found.Matched);
        Assert.True(found.LabelVisible);
        Assert.True(found.CanOpen);
        Assert.Equal(
            "FX100 1.1.1, Fictional guideline\nhttps://example.test/fx100", found.CitationText);

        var bare = Found(Result("fx100-1_1_1", number: "", section: "", updateTag: "2015"));
        Assert.Equal("FX100", bare.Reference);
        Assert.Equal("[2015]", bare.Tag);
        Assert.False(bare.PathVisible);
        Assert.Equal("Open FX100", bare.OpenName);

        var whole = Found(Result("fx100-1_1_1", trigger: ""));
        Assert.Equal("Matched: the note as a whole", whole.Matched);
        var typed = GuidanceRecommendation.From(Parse(Result("fx100-1_1_1", trigger: "")), "NICE", false);
        Assert.False(typed.MatchedVisible);

        // Only web links open
        var file = Found(Result("gout-1", url: "Gout.md", source: "text"));
        Assert.False(file.CanOpen);
        Assert.Equal("FX100 1.1.1, Fictional guideline", file.CitationText);
        Assert.False(Found(Result("x-1", url: "")).CanOpen);

        var cards = GuidanceCard.Group(
        [
            Found(Result("fx100-1_1_1", lastUpdated: "2020-10-12T09:30:00Z")),
            Found(Result("fx200-1_1_1")),
            Found(Result("fx100-1_1_2", lastUpdated: "2020-10-12T09:30:00Z")),
        ]).ToList();

        Assert.Equal(["FX100", "FX200"], cards.Select(c => c.Code));
        Assert.Equal("Fictional guideline", cards[0].Title);
        Assert.Equal(["fx100-1_1_1", "fx100-1_1_2"],
            cards[0].Recommendations.Select(r => r.ChunkId));
        Assert.Equal("Updated 12 Oct 2020 · 2 recommendations", cards[0].Meta);
        Assert.Equal("1 recommendation", cards[1].Meta);
        Assert.Equal("NICE · FX100", cards[0].Chip);
    }
}
