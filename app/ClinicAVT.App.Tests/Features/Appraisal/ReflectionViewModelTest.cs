using System.Text.Json;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Features.Appraisal;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;

namespace ClinicAVT.App.Tests.Features.Appraisal;

public class ReflectionViewModelTest
{
    private static (ReflectionViewModel Reflection, FakeEngineClient Engine) Create()
    {
        var engine = new FakeEngineClient();
        return (new ReflectionViewModel(new EngineApi(engine), new InlineDispatcher(), new FakeClipboard(), new FakeFilePicker(), new FakeDialogService(), TestSession.Status(engine)), engine);
    }

    [Fact]
    public async Task ANewEntryAsksForTheSummaryStartsEmptyWarnsOfAnIdentifierAndAStoredEntryLoadsWithoutAsking()
    {
        var (reflection, engine) = Create();

        await reflection.LoadAsync("abc", "2026-09-04T09:12:00Z");

        Assert.Equal("Elbow swelling", reflection.Title);
        Assert.Equal("September 2026", reflection.Month);
        Assert.Contains(engine.Requests, r => r.Method == "reflection/summary" && r.Params.Contains("abc"));
        Assert.Equal("A patient in their forties presented with a swollen elbow.", reflection.Summary);
        Assert.False(reflection.SummaryPending);
        Assert.Equal("", reflection.Happened);
        Assert.False(reflection.Dirty);
        Assert.False(reflection.HasReferences);
        Assert.DoesNotContain("Guidance referred to", reflection.ExportText);

        reflection.Learned = "check the temperature";
        Assert.StartsWith("Elbow swelling\nSeptember 2026\n\nCase study\n", reflection.ExportText);
        Assert.Contains("What did I learn?\ncheck the temperature", reflection.ExportText);

        Assert.False(reflection.HasWarning);
        reflection.Happened = "Mrs Patel was upset";
        Assert.Contains("Mrs Patel", reflection.Warning);

        // Stored answers and summary come back as they were
        engine.ReflectionSummary = "A patient in their thirties with a cough.";
        engine.ReflectionAnswers = ("h", "l", "n");
        await reflection.LoadAsync("abc");

        Assert.Equal(1, engine.Requests.Count(r => r.Method == "reflection/summary"));
        Assert.Equal("A patient in their thirties with a cough.", reflection.Summary);
        Assert.Equal(("h", "l", "n"), (reflection.Happened, reflection.Learned, reflection.Next));
    }

    [Fact]
    public async Task SaveSendsOnlyWhenSomethingChangedAndAStrayLineBreakIsNotAnAnswer()
    {
        var (reflection, engine) = Create();
        engine.ReflectionAnswers = ("\r\n", "", " ");
        await reflection.LoadAsync("abc");
        Assert.Equal("", reflection.Happened);
        Assert.Equal("", reflection.Next);

        await reflection.SaveAsync();
        Assert.DoesNotContain(engine.Requests, r => r.Method == "reflection/update");

        reflection.Happened = "\n";
        await reflection.SaveAsync();
        Assert.Equal("", reflection.Happened);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "reflection/update");

        reflection.Learned = "check the temperature";
        Assert.True(reflection.Dirty);
        await reflection.SaveAsync();
        var update = Assert.Single(engine.Requests, r => r.Method == "reflection/update");
        var sent = JsonDocument.Parse(update.Params).RootElement;
        Assert.Equal("abc", sent.GetProperty("id").GetString());
        Assert.Equal("check the temperature", sent.GetProperty("learned").GetString());
        Assert.Equal("", sent.GetProperty("happened").GetString());
        Assert.False(reflection.Dirty);

        await reflection.SaveAsync();
        Assert.Single(engine.Requests, r => r.Method == "reflection/update");

        reflection.Happened = "  a real answer ";
        await reflection.SaveAsync();
        Assert.Equal(2, engine.Requests.Count(r => r.Method == "reflection/update"));
        Assert.Contains("a real answer", engine.Requests.Last(r => r.Method == "reflection/update").Params);
    }

    [Fact]
    public async Task TheConsultationsGuidelinesListOnePerDocumentWithTheStoredTicksAndAnAddedDocumentNamesItsPages()
    {
        var (reflection, engine) = Create();
        engine.StoredGuidance = Guidance("ng100-1_1_1", "ng100-1_2_3", "cg79-1_1_1");
        engine.ReflectionReferences.Add(new ReflectionReference("nice:cg79", "CG79", "Fictional guideline", "https://example.test/cg79", "NICE"));
        engine.ReflectionReferences.Add(new ReflectionReference("upload:gone", "", "Old leaflet", "", "Added document, page 2"));

        await reflection.LoadAsync("abc");

        Assert.True(reflection.HasReferences);
        Assert.Equal(["nice:ng100", "nice:cg79", "upload:gone"], reflection.References.Select(r => r.Key));
        Assert.Equal([false, true, true], reflection.References.Select(r => r.Ticked));
        Assert.Equal("NG100", reflection.References[0].Reference);
        Assert.Equal("Fictional guideline", reflection.References[0].Title);
        Assert.Equal("Old leaflet", reflection.References[2].Title);
        Assert.False(reflection.Dirty);
        Assert.Contains("Guidance referred to\nCG79 Fictional guideline (NICE)", reflection.ExportText);

        // An added document is one row naming its pages
        engine.StoredGuidance = Guidance("doc7-p19", "doc7-p21", "doc7-p19b");
        engine.ReflectionReferences.Clear();
        await reflection.LoadAsync("abc");

        var row = Assert.Single(reflection.References);
        Assert.Equal("", row.Reference);
        Assert.Equal("Fictional guideline", row.Title);
        Assert.Equal("Added document, pages 19 and 21", row.Source);
    }

    [Fact]
    public async Task ATickSavesTheReferenceWithItsOwnWords()
    {
        var (reflection, engine) = Create();
        engine.StoredGuidance = Guidance("ng100-1_1_1");
        await reflection.LoadAsync("abc");
        Assert.False(reflection.Dirty);

        // The tick saves by itself without a call to SaveAsync
        reflection.References[0].Ticked = true;
        Assert.False(reflection.Dirty);

        var update = Assert.Single(engine.Requests, r => r.Method == "reflection/update");
        var sent = JsonDocument.Parse(update.Params).RootElement.GetProperty("references");
        var reference = Assert.Single(sent.EnumerateArray());
        Assert.Equal("nice:ng100", reference.GetProperty("key").GetString());
        Assert.Equal("NG100", reference.GetProperty("reference").GetString());
        Assert.Equal("Fictional guideline", reference.GetProperty("title").GetString());
        Assert.Equal("https://example.test/ng100", reference.GetProperty("link").GetString());
        Assert.Equal("NICE", reference.GetProperty("source").GetString());
        Assert.False(reflection.Dirty);

        reflection.References[0].Ticked = false;
        var cleared = engine.Requests.Last(r => r.Method == "reflection/update");
        Assert.Equal(0, JsonDocument.Parse(cleared.Params).RootElement.GetProperty("references").GetArrayLength());
    }

    [Fact]
    public async Task AddedGuidanceIsAListOfItsOwnEntriesEachSavedAndRemovable()
    {
        var (reflection, engine) = Create();
        engine.StoredGuidance = Guidance("ng100-1_1_1");
        engine.ReflectionReferences.Add(new ReflectionReference("typed:1", "", "BNF, methotrexate monitoring"));
        await reflection.LoadAsync("abc");

        var stored = Assert.Single(reflection.Added);
        Assert.Equal("BNF, methotrexate monitoring", stored.Title);
        Assert.Single(reflection.References);
        Assert.False(reflection.Dirty);
        Assert.Contains("Guidance referred to\nBNF, methotrexate monitoring\n", reflection.ExportText);

        // Enter on a typed line adds and saves it. A blank line adds nothing
        reflection.Draft = " SIGN 156 ";
        await reflection.AddDraftCommand.ExecuteAsync(null);
        Assert.Equal("", reflection.Draft);
        Assert.Equal(["BNF, methotrexate monitoring", "SIGN 156"], reflection.Added.Select(r => r.Title));
        var sent = JsonDocument.Parse(engine.Requests.Last(r => r.Method == "reflection/update").Params)
            .RootElement.GetProperty("references");
        Assert.Equal(2, sent.GetArrayLength());
        Assert.StartsWith(ReflectionViewModel.TypedKey, sent[1].GetProperty("key").GetString());
        Assert.Equal("SIGN 156", sent[1].GetProperty("title").GetString());
        Assert.False(reflection.Dirty);
        reflection.Draft = "   ";
        await reflection.AddDraftCommand.ExecuteAsync(null);
        Assert.Equal(2, reflection.Added.Count);

        // A tick and the added lines export together, ticks first
        reflection.References[0].Ticked = true;
        await reflection.SaveAsync();
        Assert.EndsWith("Fictional guideline (NICE)\nhttps://example.test/ng100\nBNF, methotrexate monitoring\nSIGN 156\n\n" + ReflectionExport.Declaration + "\n", reflection.ExportText);

        // Remove saves at once
        await ((IAsyncRelayCommand)stored.RemoveCommand).ExecuteAsync(null);
        Assert.Equal(["SIGN 156"], reflection.Added.Select(r => r.Title));
        sent = JsonDocument.Parse(engine.Requests.Last(r => r.Method == "reflection/update").Params)
            .RootElement.GetProperty("references");
        Assert.Equal(2, sent.GetArrayLength());
        Assert.Equal("SIGN 156", sent[1].GetProperty("title").GetString());
        Assert.False(reflection.Dirty);
    }

    // "ng100-1_1_1" is a guideline passage and "doc7-p19" is a page of an added document
    private static JsonElement Guidance(params string[] chunkIds) =>
        GuidanceRecords.Record(chunkIds.Select(Shown).ToArray(), searched:
        [
            GuidanceRecords.Corpus("NICE", "nice", "NICE guidelines"),
            GuidanceRecords.Corpus("", "uploads", "Your documents"),
        ]);

    private static object Shown(string chunkId)
    {
        var (code, rest) = (chunkId.Split('-')[0], chunkId.Split('-')[1]);
        return chunkId.StartsWith("doc", StringComparison.Ordinal)
            ? GuidanceRecords.Result(chunkId, trigger: "", url: "", source: "upload", number: "",
                corpus: "uploads", citation: "NICE NG100", document: 7, pages: 40,
                page: int.Parse(rest.TrimStart('p').TrimEnd('b'), System.Globalization.CultureInfo.InvariantCulture) - 1)
            : GuidanceRecords.Result(chunkId, trigger: "", url: "https://example.test/" + code, source: "nice",
                number: rest.Replace('_', '.'), corpus: "nice", citation: "NICE NG100");
    }

    [Fact]
    public async Task AFailedSummarySaysSoRewriteTriesAgainAndAnotherSessionsSummaryIsIgnored()
    {
        var (reflection, engine) = Create();
        engine.SummaryFails = true;

        await reflection.LoadAsync("abc");

        Assert.Equal("", reflection.Summary);
        Assert.Contains("the model is not loaded", reflection.SummaryProblem);
        Assert.False(reflection.SummaryPending);

        engine.SummaryFails = false;
        await reflection.RewriteSummaryCommand.ExecuteAsync(null);
        Assert.NotEqual("", reflection.Summary);
        Assert.Equal("", reflection.SummaryProblem);
        var mine = reflection.Summary;

        engine.RaiseNotification("reflection/summary",
            JsonSerializer.SerializeToElement(new { id = "other", text = "theirs" }));

        Assert.Equal(mine, reflection.Summary);
    }

    // A summary asked for while the model loads waits for it and says so. One on its way is lost
    // with the engine writing it, and the sheet stops waiting
    [Fact]
    public async Task ASummaryInFlightSaysWhatItWaitsForAndEndsWhenTheEngineGoes()
    {
        var engine = new FakeEngineClient { SummarySilent = true };
        var status = TestSession.Status(engine);
        using var reflection = new ReflectionViewModel(new EngineApi(engine), new InlineDispatcher(), new FakeClipboard(), new FakeFilePicker(), new FakeDialogService(), status);
        status.ApplyNoteModel("loading", firstUse: false);

        await reflection.LoadAsync("abc");

        Assert.True(reflection.SummaryPending);
        Assert.StartsWith("Waiting for the note model to load · 0:00", reflection.SummaryPlaceholder);
        status.ApplyNoteModel("ready", firstUse: false);
        Assert.Equal("Writing the case study", reflection.SummaryPlaceholder);

        engine.SetConnected(false);

        Assert.False(reflection.SummaryPending);
        Assert.Contains("ClinicAVT restarted", reflection.SummaryProblem);
        Assert.Equal("", reflection.SummaryPlaceholder);
    }

    [Fact]
    public async Task ATitleSavesAsTheConsultationsLabelOnlyWhenChanged()
    {
        var (reflection, engine) = Create();
        await reflection.LoadAsync("abc");

        await reflection.SaveTitleAsync();
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/label");

        reflection.Title = "  ";
        await reflection.SaveTitleAsync();
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/label");
        Assert.Equal(reflection.Month, reflection.DisplayTitle);

        reflection.Title = "Elbow swelling, missed infection";
        await reflection.SaveTitleAsync();
        var rename = Assert.Single(engine.Requests, r => r.Method == "session/label");
        Assert.Contains("missed infection", rename.Params);
        await reflection.SaveTitleAsync();
        Assert.Single(engine.Requests, r => r.Method == "session/label");
    }
}
