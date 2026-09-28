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
/// The Guidelines section's behaviour, driven through the consultation view model.
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
    private static async Task<(ConsultationViewModel Session, FakeEngineClient Engine)>
        AfterNoteAsync(ListLogger? log = null)
    {
        var (session, engine, _) = TestSession.Create(log: log);
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready", Note("the clinical note"));
        return (session, engine);
    }

    // Reopens stored consultation "abc". A record means its note was searched and null that it never was
    private static async Task<(
        ConsultationViewModel Session, FakeEngineClient Engine, NoteViewModel Note)>
        ReopenedAsync(JsonElement? record = null, ListLogger? log = null)
    {
        var (session, engine, note) = TestSession.Create(log: log);
        engine.StoredNote = "the stored note";
        engine.StoredGuidance = record;
        await session.OpenStoredSessionAsync("abc");
        return (session, engine, note);
    }

    [Fact]
    public async Task AReopenedRecordShowsFreshGoesStaleOnlyWhenTheNoteChangesAndClearsWithTheConsultation()
    {
        var (session, engine, note) =
            await ReopenedAsync(Record([Result("fx100-1_1_1"), Result("fx100-1_1_2")]));

        var guidance = session.Guidance;
        Assert.Equal(GuidanceSection.Results, guidance.Section);
        Assert.Equal(2, guidance.Cards.Single().Recommendations.Count);
        Assert.Equal("", guidance.FoundIn);
        Assert.False(guidance.Stale);
        Assert.True(guidance.HasRecord);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "guidance/search");

        await session.SaveNoteAsync();
        Assert.False(guidance.Stale);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "note/update");

        note.ClinicalNoteText = "edited";
        await session.SaveNoteAsync();
        Assert.True(guidance.Stale);
        Assert.Contains(engine.Requests, r => r.Method == "note/update");

        await session.CloseReviewAsync();
        Assert.Equal(GuidanceSection.Hidden, guidance.Section);
        Assert.Empty(guidance.Cards);
        Assert.False(guidance.Stale);
    }

    // The note-edit caption wins over changed documents, whether the record loads stale or goes stale
    [Fact]
    public async Task ChangedDocumentsMarkAStoredResultStaleWithTheirOwnCaptionUntilTheNoteIsEditedAndSearchAgainByThemselves()
    {
        var (session, engine, note) = await ReopenedAsync(Record([Result("fx100-1_1_1")]));

        engine.RaiseNotification("guidance/documentsChanged", DocumentsChanged);

        Assert.True(session.Guidance.Stale);
        Assert.True(session.Guidance.SearchAgainVisible);
        Assert.Equal(
            "Added documents changed since this guidance was found.",
            session.Guidance.StaleCaption);

        note.ClinicalNoteText = "edited";
        await session.SaveNoteAsync();

        Assert.Equal(
            "This guidance was found before your note edits.", session.Guidance.StaleCaption);

        await session.CloseReviewAsync();
        engine.StoredGuidance = Record([Result("fx100-1_1_1")], stale: true, documentsChanged: true);
        await session.OpenStoredSessionAsync("abc");

        Assert.True(session.Guidance.Stale);
        Assert.True(session.Guidance.SearchAgainVisible);
        Assert.Equal(
            "This guidance was found before your note edits.", session.Guidance.StaleCaption);

        // A note older than the documents loads stale and searches again by itself
        await session.CloseReviewAsync();
        engine.StoredGuidance = Record([], documentsChanged: true);
        session.Review.DocumentsSettle = TimeSpan.FromMilliseconds(80);
        var searches = engine.Requests.Count(r => r.Method == "guidance/search");
        await session.OpenStoredSessionAsync("abc");

        Assert.True(session.Guidance.Stale);
        Assert.Equal(
            "Added documents changed since this guidance was found.",
            session.Guidance.StaleCaption);
        await WaitUntilAsync(() => engine.Requests.Count(r => r.Method == "guidance/search") > searches);
    }

    [Fact]
    public async Task AnEmptyNoteHidesTheSectionANoteNeverSearchedSaysSoAndSoDoesAQueryWithNoMatch()
    {
        var (session, engine, _) = TestSession.Create();
        engine.StoredGuidance = Record([Result("fx100-1_1_1")]);
        await session.OpenStoredSessionAsync("abc");
        Assert.Equal(GuidanceSection.Hidden, session.Guidance.Section);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/guidance");

        await session.CloseReviewAsync();
        engine.StoredNote = "the stored note";
        engine.StoredGuidance = null;
        await session.OpenStoredSessionAsync("abc");

        Assert.Equal(GuidanceSection.NotSearched, session.Guidance.Section);
        Assert.Equal(
            "Guidance was not searched for this consultation", session.Guidance.StateCaption);
        Assert.True(session.Guidance.SearchNoteCommand.CanExecute(null));
        Assert.DoesNotContain(engine.Requests, r => r.Method == "guidance/search");

        session.Guidance.Query = "nothing here";
        await session.Guidance.SearchQueryCommand.ExecuteAsync(null);
        engine.RaiseNotification("guidance/ready", Ready(null, [], stale: null));

        Assert.Equal("Nothing came close enough to show.", session.Guidance.QueryCaption);
        Assert.False(session.Guidance.QuerySearching);
        Assert.Empty(session.Guidance.Cards);
        Assert.Equal("", session.Guidance.Summary);
        Assert.False(session.Guidance.CardsVisible);
        Assert.Equal(GuidanceSection.NotSearched, session.Guidance.Section);
        Assert.False(session.Guidance.CaptionVisible, "the query bar replaces the note caption");

        session.Guidance.ClearQueryCommand.Execute(null);
        Assert.True(session.Guidance.CaptionVisible);
    }

    [Fact]
    public async Task ATypedQueryFailsInTheQueryBarWhetherTheEngineRefusesOrGoesAway()
    {
        var log = new ListLogger();
        var (session, engine, _) = await ReopenedAsync(log: log);
        session.Guidance.Query = "gout";
        await session.Guidance.SearchQueryCommand.ExecuteAsync(null);

        engine.RaiseNotification("guidance/failed", Failed(null, "embedder gone"));

        Assert.Equal("Search failed.", session.Guidance.QueryCaption);
        Assert.Contains(
            log.Lines, e => e.Contains("embedder gone", StringComparison.Ordinal));
        Assert.Equal(GuidanceSection.NotSearched, session.Guidance.Section);

        await session.Guidance.SearchQueryCommand.ExecuteAsync(null);
        Assert.True(session.Guidance.QuerySearching);
        engine.SetConnected(false);

        Assert.False(session.Guidance.QuerySearching);
        Assert.Equal("Search failed.", session.Guidance.QueryCaption);
        Assert.True(session.Guidance.QueryShown);
    }

    [Fact]
    public async Task TheSharedFixturesLoad()
    {
        var (session, _, _) = await ReopenedAsync(Fixtures.Load("session-guidance.json")
            .GetProperty("result").GetProperty("guidance"));
        var guidance = session.Guidance;

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

        guidance.ApplyCorpora(Protocol.Parse<CorporaStatus>(Fixtures.Load("guidance-corpora.json").GetProperty("result"))!);
        Assert.Equal(GuidanceReadiness.Ready, guidance.Readiness);
        Assert.Equal(
            "nice-2026-08-25: corpus.db sha256 does not match the manifest",
            guidance.RefusedCorpora.Single());
    }

    [Fact]
    public async Task AnAddedDocumentLeadsWithItsChipAndShowInDocumentOpensThePageView()
    {
        var (session, engine) = await AfterNoteAsync();
        session.Guidance.ApplyReady(Ready("s1", [Result("fx100-1_1_1"), DocumentResult()],
            corpora: [Corpus, DocumentCorpus]));

        var cards = session.Guidance.Cards;
        Assert.Equal(2, cards.Count);
        Assert.True(cards[0].FromDocument);
        Assert.Equal("Added document", cards[0].Chip);
        Assert.Equal("BSR PMR guidelines 2009", cards[0].Title);
        Assert.Equal("5 pages · added 15 Sep 2026 · 1 recommendation", cards[0].Meta);
        Assert.False(cards[0].DividerVisible);
        Assert.Equal("From NICE", cards[1].Divider);
        Assert.Equal("1 added document · 1 guideline", session.Guidance.Summary);

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
        await session.Guidance.ShowInDocumentCommand.ExecuteAsync(found);

        Assert.True(session.PageView.Visible);
        Assert.Equal("Page 2 of 5", session.PageView.PageLabel);
        Assert.Contains("\"id\":6368831970660585267", engine.Requests[^1].Params);
        Assert.Single(session.PageView.Boxes);

        session.Guidance.Reset();
        session.PageView.Hide();
        Assert.False(session.PageView.Visible);
    }

    [Fact]
    public async Task TheNotesSearchStartsWithTheNoteTheBoxWaitsAndAnotherSessionsResultIsIgnored()
    {
        var log = new ListLogger();
        var (session, engine, _) = TestSession.Create(log: log);
        session.Guidance.Query = "gout";
        Assert.False(session.Guidance.Visible);
        Assert.False(session.Guidance.QueryBoxEnabled);
        Assert.False(session.Guidance.SearchQueryCommand.CanExecute(null));

        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        Assert.Equal(GuidanceSection.FollowsNote, session.Guidance.Section);

        engine.RaiseNotification("note/ready", Note("the clinical note"));
        Assert.Equal(GuidanceSection.Searching, session.Guidance.Section);
        Assert.False(session.Guidance.SearchEnabled);
        Assert.False(session.Guidance.QueryBoxEnabled);

        engine.RaiseNotification("guidance/ready", Ready("other", [Result("fx100-1_1_1")]));
        Assert.Equal(GuidanceSection.Searching, session.Guidance.Section);
        Assert.Empty(session.Guidance.Cards);
        Assert.Contains(
            log.Lines, e => e.Contains("other", StringComparison.Ordinal));

        engine.RaiseNotification("guidance/ready", Ready("s1", [Result("fx100-1_1_1")]));
        Assert.Equal(GuidanceSection.Results, session.Guidance.Section);
        Assert.False(session.Guidance.Stale);
    }

    // Sentence matches lead whole-note matches, both across cards and within one
    [Fact]
    public async Task ResultsAreOrderedAndEmptyUnknownCorpusAndUnstoredRepliesEachReadTheirOwnWay()
    {
        var log = new ListLogger();
        var (session, engine) = await AfterNoteAsync(log);

        engine.RaiseNotification("guidance/ready", Ready("s1",
        [
            Result("fx200-1_1_1", trigger: ""),
            Result("fx100-1_1_1", trigger: "Second sentence."),
            Result("fx200-1_1_2", trigger: "Third sentence."),
        ]));

        Assert.Equal(["FX100", "FX200"], session.Guidance.Cards.Select(c => c.Code));
        Assert.Equal(["fx100-1_1_1", "fx200-1_1_2", "fx200-1_1_1"], Shown(session.Guidance));
        Assert.Equal("2 guidelines · 3 recommendations", session.Guidance.Summary);
        Assert.True(session.Guidance.CardsVisible);
        Assert.Matches(@"^found in \d+\.\d s$", session.Guidance.FoundIn);

        engine.RaiseNotification("guidance/ready", Ready("s1", []));
        Assert.Equal(GuidanceSection.NothingMatched, session.Guidance.Section);

        engine.RaiseNotification("guidance/ready", Ready("s1", [], searched: false));
        Assert.Equal(GuidanceSection.NoCorpusAtSearch, session.Guidance.Section);
        Assert.StartsWith("No guideline documents yet", session.Guidance.StateCaption);
        Assert.True(session.Guidance.SettingsLinkVisible, "Settings holds the folder");

        // A result whose corpus was not in the searched list shows its code
        engine.RaiseNotification("guidance/ready",
            Ready("s1", [Result("gout-1", source: "text")], searched: false));
        Assert.Equal(GuidanceSection.Results, session.Guidance.Section);
        Assert.Equal("GOUT", session.Guidance.Cards.Single().Chip);

        engine.RaiseNotification("guidance/ready",
            Ready("s1", [Result("fx100-1_1_1")], storeError: "disk full"));
        Assert.Equal(GuidanceSection.Results, session.Guidance.Section);
        Assert.True(session.Guidance.NotStored);
        Assert.Contains(
            log.Lines, e => e.Contains("disk full", StringComparison.Ordinal));
    }

    [Theory]
    [InlineData("guidance/ready", GuidanceSection.Results)]
    [InlineData("guidance/failed", GuidanceSection.Failed)]
    public async Task AnOutcomeThatArrivesBeforeTheNoteStands(string method, GuidanceSection section)
    {
        var (session, engine, _) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();

        engine.RaiseNotification(method,
            method == "guidance/ready" ? Ready("s1", [Result("fx100-1_1_1")]) : Failed("s1", "embedder gone"));
        engine.RaiseNotification("note/ready", Note("the clinical note"));

        Assert.Equal(section, session.Guidance.Section);
    }

    [Fact]
    public async Task SearchAgainSavesAChangedNoteThenSearchesAndASupersededFailureChangesNothing()
    {
        var (session, engine, note) = await ReopenedAsync(Record([Result("fx100-1_1_1")]));
        note.ClinicalNoteText = "edited";
        engine.BeforeReply = method =>
        {
            if (method == "guidance/search")
            {
                engine.RaiseNotification("guidance/failed", Failed("abc", "superseded"));
            }
        };

        await session.Guidance.SearchNoteCommand.ExecuteAsync(null);

        var methods = engine.Requests.Select(r => r.Method).ToList();
        var save = methods.IndexOf("note/update");
        Assert.True(save >= 0 && save < methods.LastIndexOf("guidance/search"));
        Assert.Contains(engine.Requests,
            r => r.Method == "guidance/search"
                && r.Params.Contains("\"id\":\"abc\"", StringComparison.Ordinal));
        Assert.Equal(GuidanceSection.Searching, session.Guidance.Section);
        Assert.False(session.Guidance.Stale, "a search in flight is not stale");

        engine.RaiseNotification("guidance/ready", Ready("abc", [Result("fx100-1_1_2")]));
        Assert.Equal(GuidanceSection.Results, session.Guidance.Section);
    }

    [Fact]
    public async Task AnyOtherFailureShowsTheFailedStateAndKeepsTheDetailOffScreen()
    {
        var log = new ListLogger();
        var (session, engine) = await AfterNoteAsync(log);

        engine.RaiseNotification("guidance/failed",
            Failed("s1", "guidance embedder gte-large-int8: tokenizer ignores max_length"));

        Assert.Equal(GuidanceSection.Failed, session.Guidance.Section);
        Assert.Equal("Guidance could not be searched.", session.Guidance.StateCaption);
        Assert.Contains(
            log.Lines, e => e.Contains("tokenizer", StringComparison.Ordinal));
        Assert.DoesNotContain("tokenizer", session.Status.LatestActivity, StringComparison.Ordinal);
        Assert.True(session.Guidance.SearchNoteCommand.CanExecute(null));
    }

    [Fact]
    public async Task LosingTheEngineEndsTheSearchAndReconnectingPollsAgain()
    {
        var (session, engine) = await AfterNoteAsync();
        var polls = engine.Requests.Count(r => r.Method == "guidance/corpora");

        engine.SetConnected(false);
        Assert.Equal(GuidanceSection.NotSearched, session.Guidance.Section);

        engine.SetConnected(true);
        Assert.Equal(polls + 1, engine.Requests.Count(r => r.Method == "guidance/corpora"));
    }

    [Fact]
    public async Task ABurstOfDocumentChangesSearchesTheNoteOnceAfterTheySettle()
    {
        var (session, engine, _) = await ReopenedAsync(Record([Result("fx100-1_1_1")]));
        session.Review.DocumentsSettle = TimeSpan.FromMilliseconds(80);
        var before = engine.Requests.Count(r => r.Method == "guidance/search");

        // Thirty documents finishing in quick succession
        for (var i = 0; i < 30; i++)
        {
            engine.RaiseNotification("guidance/documentsChanged", DocumentsChanged);
        }

        Assert.True(session.Guidance.Stale);
        await WaitUntilAsync(() => engine.Requests.Count(r => r.Method == "guidance/search") != before);

        await Task.Delay(300);  // long enough for any second search to have fired
        Assert.Equal(before + 1, engine.Requests.Count(r => r.Method == "guidance/search"));
    }

    [Fact]
    public async Task DocumentChangesLeaveATypedQueryAlone()
    {
        var (session, engine, _) = await ReopenedAsync(Record([Result("fx100-1_1_1")]));
        session.Review.DocumentsSettle = TimeSpan.FromMilliseconds(50);
        session.Guidance.Query = "allopurinol";
        await session.Guidance.SearchQueryCommand.ExecuteAsync(null);
        var before = engine.Requests.Count(r => r.Method == "guidance/search");

        engine.RaiseNotification("guidance/documentsChanged", DocumentsChanged);
        await Task.Delay(300);

        Assert.Equal(before, engine.Requests.Count(r => r.Method == "guidance/search"));
    }

    [Fact]
    public void EverySearchAnnouncesItsTimingEvenWhenTheTextRepeats()
    {
        var guidance = new GuidanceViewModel(new FakeLauncher(), new FakeClipboard(), TestSession.Status());
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
        var (session, engine, note) =
            await ReopenedAsync(Record([Result("fx100-1_1_1")], stale: true));

        await note.RegenerateCommand.ExecuteAsync(null);
        Assert.Equal(GuidanceSection.FollowsNote, session.Guidance.Section);
        Assert.False(session.Guidance.Stale);
        Assert.Empty(session.Guidance.Cards);

        engine.RaiseNotification("note/ready", Note("rewritten"));
        engine.RaiseNotification("guidance/ready", Ready("abc", [Result("fx100-1_1_2")]));
        Assert.Equal(GuidanceSection.Results, session.Guidance.Section);
        Assert.False(session.Guidance.Stale);
    }

    [Fact]
    public async Task OpeningAnotherConsultationDuringTheSaveLeavesItAlone()
    {
        var (session, engine, note) = await ReopenedAsync(Record([Result("fx100-1_1_1")]));
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

        await session.Guidance.SearchNoteCommand.ExecuteAsync(null);

        Assert.DoesNotContain(engine.Requests, r => r.Method == "guidance/search");
        Assert.Equal(GuidanceSection.Results, session.Guidance.Section);
        Assert.False(session.Guidance.Stale);
        Assert.DoesNotContain(
            "Note saved", session.Status.LatestActivity, StringComparison.Ordinal);
    }

    [Fact]
    public async Task ATypedQueryStandsInForTheNotesCardsUntilCleared()
    {
        var (session, engine, _) = await ReopenedAsync(Record([Result("fx100-1_1_1")]));
        var guidance = session.Guidance;

        guidance.Query = "gout flare";
        Assert.True(guidance.SearchEnabled);
        await guidance.SearchQueryCommand.ExecuteAsync(null);
        Assert.True(guidance.QuerySearching);
        Assert.False(guidance.SearchEnabled);
        Assert.False(guidance.QueryBoxEnabled);
        Assert.Contains(engine.Requests,
            r => r.Method == "guidance/search"
                && r.Params.Contains("\"text\":\"gout flare\"", StringComparison.Ordinal));

        Assert.True(guidance.QueryShown);
        Assert.Equal("Search: 'gout flare'", guidance.QueryHeader);
        Assert.Empty(guidance.Cards);

        engine.RaiseNotification("guidance/ready",
            Ready(null, [Result("fx200-1_1_1", trigger: ""), Result("fx200-1_1_2", trigger: "")],
                stale: null));
        Assert.Equal("2 results", guidance.QueryCaption);
        Assert.Matches(@"^found in \d+\.\d s$", guidance.FoundIn);
        Assert.Equal(["fx200-1_1_1", "fx200-1_1_2"], Shown(guidance));
        Assert.All(guidance.Cards.Single().Recommendations, r => Assert.False(r.MatchedVisible));
        Assert.Equal(GuidanceSection.Results, guidance.Section);

        // The note's own result lands behind the query and shows only after Clear
        engine.RaiseNotification("guidance/ready",
            Ready("abc", [Result("fx100-1_1_2"), Result("fx100-1_1_3")]));
        Assert.Equal(["fx200-1_1_1", "fx200-1_1_2"], Shown(guidance));

        guidance.ClearQueryCommand.Execute(null);
        Assert.False(guidance.QueryShown);
        Assert.Equal("", guidance.Query);
        Assert.Equal("", guidance.FoundIn);
        Assert.Equal(["fx100-1_1_2", "fx100-1_1_3"], Shown(guidance));
    }

    [Fact]
    public async Task AQueryReplyAfterClearOrANewConsultationIsIgnored()
    {
        var (session, engine, _) = await ReopenedAsync(Record([Result("fx100-1_1_1")]));
        session.Guidance.Query = "gout";
        await session.Guidance.SearchQueryCommand.ExecuteAsync(null);
        session.Guidance.ClearQueryCommand.Execute(null);

        engine.RaiseNotification("guidance/ready", Ready(null, [], stale: null));
        Assert.Equal("", session.Guidance.QueryCaption);
        Assert.False(session.Guidance.QueryShown);

        session.Guidance.Query = "gout";
        await session.Guidance.SearchQueryCommand.ExecuteAsync(null);
        await session.CloseReviewAsync();
        engine.RaiseNotification("guidance/failed", Failed(null, "late"));
        Assert.Equal("", session.Guidance.QueryCaption);
        Assert.Empty(session.Guidance.Cards);
    }

    // Added documents can be searched without an installed corpus, so a refused one never blocks
    [Fact]
    public void ReadinessComesFromThePollThenTheNotificationAndARefusedCorpusStillSearches()
    {
        var engine = new FakeEngineClient(autoNotify: false) { GuidanceState = "loading" };
        engine.GuidanceCorpora.Clear();
        engine.GuidanceCorpora.Add(new { id = "nice", unavailable = "sha256 differs" });
        var (session, _, _) = TestSession.Create(engine: engine);
        Assert.Equal(GuidanceReadiness.Loading, session.Guidance.Readiness);

        engine.GuidanceState = "ready";
        engine.RaiseNotification("guidance/model",
            JsonSerializer.SerializeToElement(new { state = "ready", detail = (string?)null }));

        Assert.Equal(GuidanceReadiness.Ready, session.Guidance.Readiness);
        Assert.Equal(2, engine.Requests.Count(r => r.Method == "guidance/corpora"));
        Assert.False(session.Guidance.SettingsLinkVisible);
        Assert.Equal(["nice: sha256 differs"], session.Guidance.RefusedCorpora);
    }

    [Fact]
    public void AFailedPollOrAnUnavailableEmbedderReadsAsUnavailableAndOnlyTheLogSaysWhy()
    {
        var log = new ListLogger();
        var failing = new FakeEngineClient(autoNotify: false)
        {
            FailNext = m => m == "guidance/corpora" ? new IOException("pipe closed") : null,
        };
        var (polled, _, _) = TestSession.Create(engine: failing, log: log);
        Assert.Equal(GuidanceReadiness.Unavailable, polled.Guidance.Readiness);
        Assert.Contains(log.Lines, e => e.Contains("pipe closed", StringComparison.Ordinal));

        var engine = new FakeEngineClient(autoNotify: false)
        {
            GuidanceState = "unavailable",
            GuidanceDetail = "no model for embedding/default",
        };
        var (session, _, _) = TestSession.Create(engine: engine, log: log);
        var status = session.Status;

        Assert.Equal(GuidanceReadiness.Unavailable, session.Guidance.Readiness);
        Assert.Contains(
            log.Lines, e => e.Contains("embedding/default", StringComparison.Ordinal));
        Assert.DoesNotContain("embedding", status.LatestActivity, StringComparison.Ordinal);
        Assert.DoesNotContain("embedding", session.Guidance.StateCaption, StringComparison.Ordinal);
        session.Guidance.Query = "gout";
        Assert.False(session.Guidance.SearchEnabled);
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
