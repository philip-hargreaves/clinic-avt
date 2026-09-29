using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Features.Settings;

public class GuidanceDocumentsViewModelTest
{
    internal static object Document(long id, string name, string state, int chunks = 0,
        string? error = null, int pages = 0, int pagesWithoutText = 0) => new
        {
            id,
            name,
            path = name + ".txt",
            sha256 = new string('0', 64),
            mime = "text/plain",
            state,
            error,
            addedAt = "2026-09-15T09:12:44Z",
            indexedAt = state == "ready" ? "2026-09-15T09:13:02Z" : null,
            bytes = 1000,
            pages,
            pagesWithoutText,
            chunks,
        };

    [Fact]
    public void AddedDocumentsListWorkingRowsFirstThenUnreadableThenByNameAndAFailureOpensTheListOnce()
    {
        var engine = new FakeEngineClient();
        engine.GuidanceDocuments.Add(Document(1, "Gout", "ready", 41));
        var documents = TestSession.Settings(engine).Get<GuidanceDocumentsViewModel>();
        Assert.Equal("1 document", documents.DocumentsSummary);
        Assert.False(documents.DocumentsExpanded);

        engine.RaiseNotification("guidance/document", Params(Document(2, "asthma", "ready", 12)));
        engine.RaiseNotification("guidance/document", Params(Document(3, "PMR", "indexing")));
        engine.RaiseNotification("guidance/document", Params(Document(4, "Letter", "failed", error: "patientData")));

        Assert.Equal(["PMR", "Letter", "asthma", "Gout"], documents.Documents.Select(d => d.Name));
        Assert.Equal("Waiting", documents.Documents[0].Detail);
        Assert.True(documents.Documents[0].Waiting);
        Assert.StartsWith("Not searched: this looks like a document about a patient.",
            documents.Documents[1].Detail);
        Assert.True(documents.Documents[1].Failed);
        Assert.Equal("12 passages · added 15 Sep 2026", documents.Documents[2].Detail);
        Assert.Equal("4 documents · reading 1, 1 could not be read", documents.DocumentsSummary);
        Assert.True(documents.DocumentsExpanded, "a failure opens the list");
        Assert.True(documents.DocumentsPresent);
        Assert.True(documents.AddDocumentsCommand.CanExecute(null));

        documents.DocumentsExpanded = false;
        engine.RaiseNotification("guidance/document", Params(Document(3, "PMR", "ready", 10)));
        Assert.False(documents.DocumentsExpanded, "the same failure does not reopen it");
    }

    [Fact]
    public async Task AddingDocumentsSendsThePathsAndCountsWhatWasSkipped()
    {
        var engine = new FakeEngineClient();
        engine.AddedDocuments.Add(Document(5, "PMR pathway", "indexing"));
        engine.SkippedDocuments.Add(new { path = @"C:\g\scan.pdf", reason = "unsupported" });
        engine.SkippedDocuments.Add(new { path = @"C:\g\empty.txt", reason = "unreadable" });
        var picker = new FakeFilePicker
        {
            Files = [@"C:\g\PMR pathway.txt", @"C:\g\scan.pdf", @"C:\g\empty.txt"],
        };
        var documents = TestSession.Settings(engine, picker: picker).Get<GuidanceDocumentsViewModel>();

        await documents.AddDocumentsCommand.ExecuteAsync(null);

        var request = Assert.Single(engine.Requests, r => r.Method == "guidance/documents/add");
        Assert.Contains("PMR pathway.txt", request.Params);
        var row = Assert.Single(documents.Documents);
        Assert.Equal("PMR pathway", row.Name);
        Assert.True(row.Working);
        Assert.Equal("1 skipped, not PDF or text · 1 could not be read", documents.DocumentsCaption);
    }

    [Fact]
    public async Task RowsFollowTheEngineAndRemoveAlwaysConfirmsBecauseItBinsTheFile()
    {
        var engine = new FakeEngineClient();
        engine.GuidanceDocuments.Add(Document(3, "PMR", "indexing"));
        var asked = 0;
        var dialogs = new FakeDialogService { OnConfirm = () => asked++ };
        var documents = TestSession.Settings(engine, dialogs: dialogs).Get<GuidanceDocumentsViewModel>();
        var row = Assert.Single(documents.Documents);

        engine.RaiseNotification("guidance/progress",
            Params(new { id = 3, phase = "paused", done = 0, total = 10 }));
        Assert.Equal("Waiting for the consultation to finish", row.Detail);
        engine.RaiseNotification("guidance/progress",
            Params(new { id = 3, phase = "preparing", done = 3, total = 10 }));
        Assert.Equal("Preparing 3 of 10 passages", row.Detail);
        Assert.Equal(0.3, row.Progress, 3);
        Assert.False(row.Waiting);

        // Removing a document that is still being read asks first, since it bins the file
        await documents.RemoveDocumentCommand.ExecuteAsync(row);
        Assert.Equal(1, asked);
        Assert.Contains(engine.Requests,
            r => r.Method == "guidance/documents/remove" && r.Params == "{\"id\":3}");

        engine.RaiseNotification("guidance/document", Params(Document(3, "PMR", "ready", 10)));
        Assert.Equal("10 passages · added 15 Sep 2026", row.Detail);
        Assert.False(row.Working);
        Assert.DoesNotContain("·", documents.DocumentsSummary);

        await documents.RemoveDocumentCommand.ExecuteAsync(row);
        Assert.Equal(2, asked);

        engine.RaiseNotification("guidance/document", Params(Document(3, "PMR", "removed", 10)));
        Assert.Empty(documents.Documents);
        Assert.False(documents.DocumentsPresent);
    }

    // Copying a file into the folder needs no embedder, so Add is always available
    [Fact]
    public void TheFolderIsListedAndAddableBeforeTheModelLoadsAnUnreachableOneRemovesNothingAndOneDriveIsRecognised()
    {
        var engine = new FakeEngineClient { GuidanceState = "loading" };
        engine.GuidanceDocuments.Add(Document(1, "Gout", "ready", 41));
        var documents = TestSession.Settings(engine).Get<GuidanceDocumentsViewModel>();

        Assert.True(documents.AddDocumentsCommand.CanExecute(null));
        Assert.Single(documents.Documents);
        Assert.Equal(@"C:\Users\clinician\Documents\ClinicAVT guidelines", documents.GuidelinesFolder);
        Assert.False(documents.FolderMissing);

        engine.GuidelinesFolderFound = false;
        engine.UnsupportedFiles = 2;
        var unreachable = TestSession.Settings(engine).Get<GuidanceDocumentsViewModel>();

        Assert.True(unreachable.FolderMissing);
        Assert.Single(unreachable.Documents);
        Assert.Equal("2 other files are not searched, not PDF or text", unreachable.DocumentsCaption);

        var roots = new[] { @"C:\Users\p\OneDrive", @"C:\Users\p\OneDrive - UCL" };
        Assert.True(OneDrive.Holds(@"C:\Users\p\onedrive - ucl\Documents\ClinicAVT guidelines", roots));
        Assert.False(OneDrive.Holds(@"C:\Users\p\Documents\ClinicAVT guidelines", roots));
        Assert.False(OneDrive.Holds("", roots));
    }
}
