using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Demo;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Features.Consultation;

/// <summary>A written case standing in as the note of a demo record.</summary>
public class ExampleCaseTest
{
    private static readonly DemoCase Gout = new("Case 3, gout", "38-year-old man with recurrent effusions.");

    private static (ConsultationViewModel Session, FakeEngineClient Engine, NoteViewModel Note) Create() =>
        TestSession.Create(exampleCases: [Gout]);

    [Fact]
    public void CasesParseAsTitledBlocksAndTheShippedFileLoads()
    {
        var cases = DemoCases.Parse("""
            Case 1, rheumatoid arthritis
            ----------------------------
            65-year-old man, three-month history of stiffness.
            Plan: refer urgently.

            Case 2, gout
            ------------
            38-year-old man.
            """);

        Assert.Equal(2, cases.Count);
        Assert.Equal("Case 1, rheumatoid arthritis", cases[0].Title);
        Assert.Equal("65-year-old man, three-month history of stiffness. Plan: refer urgently.", cases[0].Text);
        Assert.Equal("38-year-old man.", cases[1].Text);
        Assert.Empty(DemoCases.Parse(""));

        var shipped = DemoCases.Load();
        Assert.Equal(4, shipped.Count);
        Assert.All(shipped, c => Assert.StartsWith("Case ", c.Title));
        Assert.All(shipped, c => Assert.True(c.Text.Split(' ').Length > 60, c.Title));
    }

    [Fact]
    public async Task ACaseStandsInIsSearchedTheOriginalComesBackLeavingWritesItBackAndARealRecordNeverOffersOne()
    {
        var (session, engine, note) = Create();
        engine.StoredNote = "the stored note";
        Assert.False(note.ExampleCasesVisible);
        Assert.True(await session.OpenStoredSessionAsync("s-copy", demo: true));
        Assert.True(note.ExampleCasesVisible);
        Assert.Equal(["Original note", "Case 3, gout"], note.ExampleCaseTitles);
        var original = note.ClinicalNoteText;

        note.ExampleCaseIndex = 1;
        Assert.True(session.Review.ExampleShown);
        Assert.Equal(Gout.Text, note.ClinicalNoteText);
        var saved = engine.Requests.Single(r => r.Method == "note/update");
        Assert.Contains("recurrent effusions", saved.Params);
        var search = engine.Requests.Single(r => r.Method == "guidance/search");
        Assert.Contains("s-copy", search.Params);

        // Switching back to the original saves and searches it again
        note.ExampleCaseIndex = 0;
        Assert.False(session.Review.ExampleShown);
        Assert.Equal(original, note.ClinicalNoteText);
        var saves = engine.Requests.Where(r => r.Method == "note/update").ToList();
        Assert.Equal(2, saves.Count);
        Assert.Contains(original, saves[1].Params);
        Assert.Equal(2, engine.Requests.Count(r => r.Method == "guidance/search"));

        note.ExampleCaseIndex = 1;
        await session.CloseReviewAsync();
        Assert.False(note.ExampleCasesVisible);
        Assert.Equal(-1, note.ExampleCaseIndex);
        Assert.False(session.Review.ExampleShown);
        var last = engine.Requests.Last(r => r.Method == "note/update");
        Assert.Contains(original, last.Params);

        // A real record never offers a case
        var updates = engine.Requests.Count(r => r.Method == "note/update");
        Assert.True(await session.OpenStoredSessionAsync("s-real"));
        Assert.False(note.ExampleCasesVisible);
        note.ExampleCaseIndex = 1;
        Assert.NotEqual(Gout.Text, note.ClinicalNoteText);
        Assert.Equal(updates, engine.Requests.Count(r => r.Method == "note/update"));
    }

    [Fact]
    public async Task ARegenerateClearsThePickerAndEditingAnExampleMakesItTheNote()
    {
        var (session, engine, note) = Create();
        Assert.True(await session.OpenStoredSessionAsync("s-copy", demo: true));
        note.ExampleCaseIndex = 1;

        await session.RegenerateNoteAsync();
        Assert.Equal(-1, note.ExampleCaseIndex);
        Assert.Contains(engine.Requests, r => r.Method == "note/regenerate");

        note.ExampleCaseIndex = 1;
        Assert.True(session.Review.ExampleShown);
        note.EditNoteCommand.Execute(null);
        Assert.False(session.Review.ExampleShown);
        Assert.Equal(Gout.Text, note.ClinicalNoteText);
    }
}
