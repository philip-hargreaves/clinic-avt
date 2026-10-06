using Microsoft.Extensions.DependencyInjection;
using ClinicAVT.App.Core.Features.Appraisal;
using ClinicAVT.App.Core.Features.Backup;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;

namespace ClinicAVT.App.Tests.Features.Appraisal;

public class ExportReflectionsTest
{
    private static readonly ReflectionReference Nice = new(
        "nice:NG100", "NG100", "Rheumatoid arthritis in adults", "https://www.nice.org.uk/guidance/ng100", "NICE");

    // Today is 27 September 2026 in London
    private static FakeEngineClient Engine()
    {
        var engine = new FakeEngineClient();
        engine.Reflections.Add(("a", "2026-09-04T09:12:00Z", "Elbow swelling", "check the temperature", "A patient in their forties."));
        engine.Reflections.Add(("d", "2025-11-03T10:00:00Z", "Back pain", "", "A patient in their sixties."));
        // 1 Sep in London
        engine.Reflections.Add(("b", "2026-08-31T23:30:00Z", "Cough", "ask about smoking", ""));
        engine.Reflections.Add(("c", "2026-08-10T10:00:00Z", "", "listen longer", ""));
        engine.Reflections.Add(("s", "2026-09-10T10:00:00Z", "Sample", "a seeded sample", ""));
        engine.SampleReflections.Add("s");
        engine.ReflectionReferences.Add(Nice);
        return engine;
    }

    private static ExportReflectionsViewModel Dialog(
        FakeEngineClient engine, FakeFilePicker picker, FakeLauncher? launcher = null, FakeTextFiles? files = null) =>
        new TestShell(engine, time: FakeTimeProvider.London(), configure: services =>
        {
            services.AddSingleton<IFilePicker>(picker);
            services.AddSingleton<ILauncher>(launcher ?? new FakeLauncher());
            services.AddSingleton<ITextFiles>(files ?? new FakeTextFiles());
        }).Create<ExportReflectionsViewModel>();

    private static string TempFile() => Path.Combine(Path.GetTempPath(), Path.GetRandomFileName() + ".txt");

    [Fact]
    public async Task ThePeriodPicksItsReflectionsNewestFirstAndTheCountLineSaysHowMany()
    {
        var export = Dialog(Engine(), new FakeFilePicker());
        await export.LoadAsync();

        // Samples are left out, as in a backup
        Assert.Equal("4 reflections on this computer.", export.CountLine);
        Assert.Equal(["a", "b", "c", "d"], export.Chosen().Select(r => r.Id));
        Assert.True(export.PrimaryEnabled);
        Assert.Equal("Choose where to save", export.PrimaryText);
        Assert.Equal("Cancel", export.CloseText);

        export.PeriodIndex = 0;
        Assert.Equal("This month (September)", export.PeriodOptions[0]);
        Assert.Equal("2 reflections, 1 Sep to 30 Sep 2026.", export.CountLine);
        Assert.Equal(["a", "b"], export.Chosen().Select(r => r.Id));

        export.PeriodIndex = 1;
        Assert.Equal("1 reflection, 1 Aug to 31 Aug 2026.", export.CountLine);
        Assert.Equal(["c"], export.Chosen().Select(r => r.Id));

        export.PeriodIndex = BackupPeriod.ChooseDatesIndex;
        Assert.Equal("Choose the first and last day.", export.CountLine);
        Assert.False(export.PrimaryEnabled);
        export.ChosenFrom = new DateTimeOffset(2026, 9, 20, 0, 0, 0, TimeSpan.Zero);
        export.ChosenTo = new DateTimeOffset(2026, 9, 1, 0, 0, 0, TimeSpan.Zero);
        Assert.Equal("The last day is before the first.", export.CountLine);
        Assert.False(export.PrimaryEnabled);
        export.ChosenTo = new DateTimeOffset(2026, 9, 26, 0, 0, 0, TimeSpan.Zero);
        Assert.Equal("There are no reflections in this period.", export.CountLine);
        Assert.False(export.PrimaryEnabled);
        export.ChosenFrom = new DateTimeOffset(2025, 11, 3, 0, 0, 0, TimeSpan.Zero);
        export.ChosenTo = new DateTimeOffset(2026, 9, 1, 0, 0, 0, TimeSpan.Zero);
        Assert.Equal("3 reflections, 3 Nov 2025 to 1 Sep 2026.", export.CountLine);
        export.ChosenTo = new DateTimeOffset(2026, 8, 31, 0, 0, 0, TimeSpan.Zero);
        // b was 1 Sep in London
        Assert.Equal("2 reflections, 3 Nov 2025 to 31 Aug 2026.", export.CountLine);
        Assert.Equal(["c", "d"], export.Chosen().Select(r => r.Id));
        Assert.True(export.PrimaryEnabled);
    }

    [Fact]
    public async Task ACancelledSaveChangesNothingAndTheFileIsTheWarningThenEachReflectionAsCopyWritesIt()
    {
        var path = TempFile();
        var picker = new FakeFilePicker();
        var launcher = new FakeLauncher();
        var files = new FakeTextFiles();
        var engine = Engine();
        var export = Dialog(engine, picker, launcher, files);
        await export.LoadAsync();
        export.PeriodIndex = 1;

        await export.PrimaryCommand.ExecuteAsync(null);
        Assert.Equal(BackupStep.Setup, export.Step);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "reflection/get");
        Assert.Equal("", export.Error);
        Assert.True(export.PrimaryEnabled);

        picker.SavePath = path;
        try
        {
            await export.PrimaryCommand.ExecuteAsync(null);

            Assert.Equal(["ClinicAVT reflections 1 Aug to 31 Aug 2026"], picker.SuggestedNames.Distinct());
            // An untitled reflection takes the month as its title, as Copy does
            Assert.Equal(
                "Check each summary for identifying details before sharing.\n"
                + "\n"
                + "August 2026\n"
                + "August 2026\n"
                + "\n"
                + "Guidance referred to\n"
                + "NG100 Rheumatoid arthritis in adults (NICE)\n"
                + "https://www.nice.org.uk/guidance/ng100\n"
                + "\n"
                + "What did I learn?\n"
                + "listen longer\n",
                files.Written[path]);
            Assert.Equal(BackupStep.Done, export.Step);
            Assert.Equal("1 reflection saved.", export.DoneLine);
            Assert.Equal($"Saved as {Path.GetFileNameWithoutExtension(path)} in {Path.GetFileName(Path.GetTempPath().TrimEnd('\\'))}.", export.SavedLine);
            Assert.Equal("Done", export.CloseText);
            Assert.Equal("", export.PrimaryText);

            export.ShowInFolderCommand.Execute(null);
            Assert.Equal([Path.GetDirectoryName(path)!], launcher.Folders);
        }
        finally
        {
            File.Delete(path);
        }

        // Everything, newest first, one rule between each
        var all = Dialog(engine, picker, files: files);
        await all.LoadAsync();
        try
        {
            await all.PrimaryCommand.ExecuteAsync(null);

            Assert.Equal("ClinicAVT reflections 27 Sep 2026", picker.SuggestedNames[^1]);
            var text = files.Written[path];
            Assert.StartsWith("Check each summary for identifying details before sharing.\n\n", text);
            var rule = "\n----------------------------------------\n\n";
            Assert.Equal(
                ExportReflectionsViewModel.Warning + "\n\n" + string.Join(rule,
                    Entry("Elbow swelling", "September 2026", "A patient in their forties.", "check the temperature"),
                    Entry("Cough", "September 2026", "", "ask about smoking"),
                    Entry("August 2026", "August 2026", "", "listen longer"),
                    Entry("Back pain", "November 2025", "A patient in their sixties.", "")),
                text);
            Assert.DoesNotContain("Sample", text);
            Assert.Equal("4 reflections saved.", all.DoneLine);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public async Task WithNoReflectionsThePageCannotExportAndTheDialogSaysThereAreNone()
    {
        var engine = new FakeEngineClient();
        engine.Reflections.Add(("s", "2026-09-10T10:00:00Z", "Sample", "a seeded sample", ""));
        engine.SampleReflections.Add("s");
        var dialogs = new FakeDialogService();
        var page = new TestShell(engine, configure: services => services.AddSingleton<IDialogService>(dialogs))
            .Get<AppraisalsViewModel>();

        Assert.False(page.ExportReflectionsCommand.CanExecute(null));
        await page.RefreshAsync();
        Assert.False(page.ExportReflectionsCommand.CanExecute(null));  // samples only

        var export = Dialog(engine, new FakeFilePicker());
        await export.LoadAsync();
        Assert.Equal("There are no reflections to export.", export.CountLine);
        Assert.False(export.PrimaryEnabled);

        engine.Reflections.Add(("a", "2026-09-04T09:12:00Z", "Elbow swelling", "check the temperature", ""));
        await page.RefreshAsync();
        Assert.True(page.ExportReflectionsCommand.CanExecute(null));
        await page.ExportReflectionsCommand.ExecuteAsync(null);
        Assert.Equal(1, dialogs.ExportsRun);
    }

    private static string Entry(string title, string month, string summary, string learned) =>
        ReflectionExport.Format(new ReflectionEntry(title, month, summary, "", learned, "") { References = [Nice] });
}
