using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Examples;
using ClinicAVT.App.Tests.Support;

namespace ClinicAVT.App.Tests.Features.Examples;

/// <summary>A written case standing in as the note of a sample record.</summary>
public class ExampleCaseTest
{
    private static readonly ExampleCase Gout = new("Case 3, gout", "38-year-old man with recurrent effusions.");

    private static TestShell Create() => TestSession.Create(exampleCases: [Gout]);

    [Fact]
    public async Task ACaseStandsInIsSearchedTheOriginalComesBackLeavingWritesItBackAndARealRecordNeverOffersOne()
    {
        var shell = Create();
        var (session, engine, note) = shell;
        engine.StoredNote = "the stored note";
        Assert.False(shell.Examples.ExampleCasesVisible);
        Assert.True(await session.OpenStoredSessionAsync("s-copy", sample: true));
        Assert.True(shell.Examples.ExampleCasesVisible);
        Assert.Equal(["Original note", "Case 3, gout"], shell.Examples.ExampleCaseTitles);
        var original = note.ClinicalNoteText;

        shell.Examples.ExampleCaseIndex = 1;
        Assert.True(shell.Examples.ExampleShown);
        Assert.Equal(Gout.Text, note.ClinicalNoteText);
        var saved = engine.Requests.Single(r => r.Method == "note/update");
        Assert.Contains("recurrent effusions", saved.Params);
        var search = engine.Requests.Single(r => r.Method == "guidance/search");
        Assert.Contains("s-copy", search.Params);

        // Switching back to the original saves and searches it again
        shell.Examples.ExampleCaseIndex = 0;
        Assert.False(shell.Examples.ExampleShown);
        Assert.Equal(original, note.ClinicalNoteText);
        var saves = engine.Requests.Where(r => r.Method == "note/update").ToList();
        Assert.Equal(2, saves.Count);
        Assert.Contains(original, saves[1].Params);
        Assert.Equal(2, engine.Requests.Count(r => r.Method == "guidance/search"));

        shell.Examples.ExampleCaseIndex = 1;
        await session.CloseReviewAsync();
        Assert.False(shell.Examples.ExampleCasesVisible);
        Assert.Equal(-1, shell.Examples.ExampleCaseIndex);
        Assert.False(shell.Examples.ExampleShown);
        var last = engine.Requests.Last(r => r.Method == "note/update");
        Assert.Contains(original, last.Params);

        // A real record never offers a case
        var updates = engine.Requests.Count(r => r.Method == "note/update");
        Assert.True(await session.OpenStoredSessionAsync("s-real"));
        Assert.False(shell.Examples.ExampleCasesVisible);
        shell.Examples.ExampleCaseIndex = 1;
        Assert.NotEqual(Gout.Text, note.ClinicalNoteText);
        Assert.Equal(updates, engine.Requests.Count(r => r.Method == "note/update"));
    }

    [Fact]
    public async Task ARegenerateClearsThePickerAndEditingAnExampleMakesItTheNote()
    {
        var shell = Create();
        var (session, engine, note) = shell;
        Assert.True(await session.OpenStoredSessionAsync("s-copy", sample: true));
        shell.Examples.ExampleCaseIndex = 1;

        await shell.Get<DocumentActions>().RegenerateNoteAsync();
        Assert.Equal(-1, shell.Examples.ExampleCaseIndex);
        Assert.Contains(engine.Requests, r => r.Method == "note/regenerate");

        shell.Examples.ExampleCaseIndex = 1;
        Assert.True(shell.Examples.ExampleShown);
        note.EditNoteCommand.Execute(null);
        Assert.False(shell.Examples.ExampleShown);
        Assert.Equal(Gout.Text, note.ClinicalNoteText);
    }
}
