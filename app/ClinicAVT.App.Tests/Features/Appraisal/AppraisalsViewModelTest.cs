using ClinicAVT.App.Core.Features.Appraisal;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Features.Appraisal;

public class AppraisalsViewModelTest
{
    private static (AppraisalsViewModel Page, FakeEngineClient Engine) Create()
    {
        var shell = CreateShell();
        return (shell.Get<AppraisalsViewModel>(), shell.Engine);
    }

    private static TestShell CreateShell()
    {
        var engine = new FakeEngineClient();
        engine.Reflections.Add(("a", "2026-09-04T09:12:00Z", "Elbow swelling", "check the temperature\nand more", "A patient in their forties."));
        engine.Reflections.Add(("b", "2026-09-01T14:00:00Z", "Cough", "ask about smoking", ""));
        engine.Reflections.Add(("c", "2026-06-20T10:00:00Z", "Back pain", "", "A patient in their sixties with back pain."));
        engine.Reflections.Add(("d", "2025-11-03T10:00:00Z", "", "listen longer", ""));
        engine.SampleReflections.Add("c");
        return new TestShell(engine);
    }

    [Fact]
    public async Task TheJournalShowsTheLatestYearNewestFirstAMonthNarrowsItAndEarlierYearsAreAStepAway()
    {
        var (page, _) = Create();

        await page.RefreshAsync();

        Assert.Equal(2026, page.Year);
        Assert.False(page.Empty);
        Assert.Equal(["a", "b", "c"], page.Cards.Select(c => c.Id));
        Assert.Equal([true, false, true], page.Cards.Select(c => c.StartsMonth));
        Assert.Equal("A patient in their forties.", page.Cards[0].Line);
        // Without a case study the line is the clinician's words
        Assert.Equal("ask about smoking", page.Cards[1].Line);
        Assert.Equal("A patient in their sixties with back pain.", page.Cards[2].Line);
        Assert.Equal([false, false, true], page.Cards.Select(c => c.Sample));
        Assert.Equal("September 2026", page.Cards[0].MonthLabel);
        Assert.Equal("September", page.Cards[0].MonthHeading);
        Assert.Equal(12, page.Months.Count);
        Assert.Equal(2, page.Months[8].Count);  // September
        Assert.True(page.Months[5].Filled);     // June
        Assert.False(page.Months[0].Filled);
        Assert.Equal("Sep", page.Months[8].Name);
        Assert.Equal(DateTimeOffset.Now.Year == 2026 ? 1 : 0, page.Months.Count(m => m.Current));

        page.ToggleMonth(9);
        Assert.Equal(9, page.MonthFilter);
        Assert.Equal(["a", "b"], page.Cards.Select(c => c.Id));
        Assert.True(page.Months[8].Selected);

        page.ToggleMonth(6);
        Assert.Equal(["c"], page.Cards.Select(c => c.Id));
        page.ToggleMonth(6);
        Assert.Equal(0, page.MonthFilter);
        Assert.Equal(3, page.Cards.Count);

        page.ToggleMonth(1);  // an empty month changes nothing
        Assert.Equal(0, page.MonthFilter);

        page.ToggleMonth(9);
        page.ShowWholeYearCommand.Execute(null);
        Assert.Equal(0, page.MonthFilter);
        Assert.Equal(3, page.Cards.Count);

        page.ToggleMonth(9);
        page.Search = "smok";
        Assert.Equal(["b"], page.Cards.Select(c => c.Id));  // search within the month
        page.Search = "";

        // In an earlier year an untitled entry shows its day. A new year starts unfiltered
        Assert.True(page.PreviousYearCommand.CanExecute(null));
        Assert.False(page.NextYearCommand.CanExecute(null));
        await page.PreviousYearCommand.ExecuteAsync(null);
        Assert.Equal(0, page.MonthFilter);
        Assert.Equal(2025, page.Year);
        var card = Assert.Single(page.Cards);
        Assert.Equal("3 November", card.Title);
        Assert.True(page.NextYearCommand.CanExecute(null));
    }

    [Fact]
    public async Task SearchFiltersTitleWordsAndCaseStudyAndOpeningACardEditsSavesAndMakesItsAnswersSearchable()
    {
        var (page, engine) = Create();
        engine.ReflectionAnswers = ("the line kept dropping", "check the temperature", "book a video review");
        engine.ReflectionSummary = "A patient in their forties.";
        await page.RefreshAsync();

        page.Search = "smok";
        Assert.Equal(["b"], page.Cards.Select(c => c.Id));
        page.Search = "back";
        Assert.Equal(["c"], page.Cards.Select(c => c.Id));
        page.Search = "sixties";
        Assert.Equal(["c"], page.Cards.Select(c => c.Id));  // the case study is searched too
        page.Search = "patient";
        Assert.Equal(["a", "c"], page.Cards.Select(c => c.Id));
        page.Search = "";
        Assert.Equal(3, page.Cards.Count);

        // Opening loads the editor. Closing saves and refreshes the line
        var card = page.Cards[0];
        await page.ToggleAsync(card);
        Assert.True(card.Expanded);
        Assert.NotNull(card.Editor);
        Assert.Equal("check the temperature", card.Editor!.Learned);

        card.Editor.Learned = "check the temperature and the pulse";
        card.Editor.CaseStudy.Summary = "A patient in their forties with a hot elbow.";
        await page.ToggleAsync(card);

        Assert.False(card.Expanded);
        Assert.Null(card.Editor);
        Assert.Equal("check the temperature and the pulse", card.Learned);
        Assert.Equal("A patient in their forties with a hot elbow.", card.Line);
        Assert.Contains(engine.Requests, r => r.Method == "reflection/update" && r.Params.Contains("pulse"));

        // Every answer of an opened card becomes searchable
        page.Search = "video review";
        Assert.Equal(["a"], page.Cards.Select(c => c.Id));
        page.Search = "dropping";
        Assert.Equal(["a"], page.Cards.Select(c => c.Id));
        page.Search = "";

        // Opening another card closes the open one
        await page.ToggleAsync(page.Cards[1]);
        await page.ToggleAsync(page.Cards[2]);
        Assert.False(page.Cards[1].Expanded);
        Assert.True(page.Cards[2].Expanded);
    }

    [Fact]
    public async Task ARetitleRenamesTheConsultationAnEmptySheetKeepsItsCardAndRemovingDropsIt()
    {
        var (page, engine) = Create();
        await page.RefreshAsync();
        var card = page.Cards[2];  // Back pain has a summary and no answers

        await page.ToggleAsync(card);
        card.Editor!.Title = "Back pain, missed red flags";
        await page.ToggleAsync(card);

        Assert.Contains(engine.Requests,
            r => r.Method == "session/label" && r.Params.Contains("missed red flags"));
        Assert.Equal("Back pain, missed red flags", card.Title);
        Assert.Equal(3, page.Cards.Count);
        Assert.Contains(card, page.Cards);

        await page.DeleteAsync(page.Cards[0]);

        Assert.Contains(engine.Requests, r => r.Method == "reflection/delete" && r.Params.Contains("\"a\""));
        Assert.Equal(["b", "c"], page.Cards.Select(c => c.Id));
    }

    [Fact]
    public async Task RemovingAReflectionAsksFirstAndKeepLeavesIt()
    {
        var shell = CreateShell();
        var page = shell.Get<AppraisalsViewModel>();
        var engine = shell.Engine;
        var asked = 0;
        shell.Dialogs.OnConfirm = () => asked++;
        shell.Dialogs.Answer = false;
        await page.RefreshAsync();

        await page.DeleteAsync(page.Cards[0]);

        Assert.Equal(1, asked);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "reflection/delete");
        Assert.Equal(3, page.Cards.Count);
    }

    [Fact]
    public async Task NothingWrittenYetIsSaidPlainly()
    {
        var page = new TestShell(new FakeEngineClient()).Get<AppraisalsViewModel>();

        await page.RefreshAsync();

        Assert.True(page.Empty);
        Assert.Empty(page.Cards);
        Assert.Equal(DateTimeOffset.Now.Year, page.Year);
    }
}
