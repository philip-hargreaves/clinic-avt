using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>The clinician's own documents in the guidelines folder, which guidance also searches.</summary>
public sealed partial class GuidanceDocumentsViewModel : ObservableObject
{
    private readonly IGuidanceApi _engine;
    private readonly IStatusLine _status;
    private readonly IDialogService _dialogs;
    private readonly IFilePicker _picker;
    private readonly ILauncher _launcher;
    private readonly IOneDriveFolders _oneDrive;
    private bool _documentsNeededAttention;

    public GuidanceDocumentsViewModel(
        IGuidanceApi engine, IEngineEvents events, IStatusLine status, IDialogService dialogs,
        IFilePicker picker, ILauncher launcher, IOneDriveFolders oneDrive)
    {
        _engine = engine;
        _status = status;
        _dialogs = dialogs;
        _picker = picker;
        _launcher = launcher;
        _oneDrive = oneDrive;
        events.OnConnected(() => _ = LoadDocumentsAsync());
        events.Subscribe<EngineNotification>(Apply);
    }

    /// <summary>
    /// The documents in the guidelines folder, the batch in progress first and then by name.
    /// </summary>
    public ObservableCollection<DocumentRow> Documents { get; } = [];

    /// <summary>The guidelines folder the engine watches.</summary>
    [ObservableProperty]
    public partial string GuidelinesFolder { get; private set; } = "";

    /// <summary>The folder and its parent were out of reach at the last scan.</summary>
    [ObservableProperty]
    public partial bool FolderMissing { get; private set; }

    /// <summary>The folder sits inside OneDrive, so the documents sync to the cloud.</summary>
    [ObservableProperty]
    public partial bool FolderInOneDrive { get; private set; }

    /// <summary>What the last add skipped, or why nothing could be added.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DocumentsCaptionVisible))]
    public partial string DocumentsCaption { get; set; } = "";

    public bool DocumentsCaptionVisible => DocumentsCaption.Length > 0;

    /// <summary>"32 documents", then what is still being read or could not be.</summary>
    public string DocumentsSummary
    {
        get
        {
            var working = Documents.Count(r => r.Working);
            var failed = Documents.Count(r => r.Failed);
            var count = Words.Count(Documents.Count, "document");
            return (working, failed) switch
            {
                (0, 0) => count,
                (_, 0) => $"{count} · reading {working}",
                (0, _) => $"{count} · {failed} could not be read",
                _ => $"{count} · reading {working}, {failed} could not be read",
            };
        }
    }

    /// <summary>The list is closed on every launch. A failure opens it once.</summary>
    [ObservableProperty]
    public partial bool DocumentsExpanded { get; set; }

    public bool DocumentsPresent => Documents.Count > 0;

    /// <summary>A document changed, or an ingest reported progress.</summary>
    private void Apply(EngineNotification notification)
    {
        switch (notification)
        {
            case GuidanceModelChanged:
                _ = LoadDocumentsAsync();
                break;
            case GuidanceDocumentChanged changed:
                Upsert(changed.Document);
                break;
            case GuidanceProgress progress:
                Documents.FirstOrDefault(r => r.Id == progress.Id)?.ApplyProgress(progress);
                break;
            default:
                break;
        }
    }

    [RelayCommand]
    private void OpenFolder()
    {
        if (GuidelinesFolder.Length > 0)
        {
            _launcher.RevealFolder(GuidelinesFolder);
        }
    }

    [RelayCommand]
    private async Task AddDocuments()
    {
        var paths = await _picker.PickFilesAsync([".pdf", ".txt", ".md"]).ConfigureAwait(true);
        if (paths.Count > 0)
        {
            await AddPathsAsync(paths).ConfigureAwait(true);
        }
    }

    private async Task AddPathsAsync(IReadOnlyList<string> paths)
    {
        if (!_engine.Connected)
        {
            return;
        }

        if (!await EngineCall.LogAsync(_status, "guidance/documents/add", async () =>
            {
                var added = await _engine.AddDocumentsAsync(paths).ConfigureAwait(true);
                foreach (var document in added.Documents)
                {
                    Upsert(document);
                }

                DocumentsCaption = SkippedCaption(added.Skipped);
            }).ConfigureAwait(true))
        {
            DocumentsCaption = "The documents could not be added";
        }
    }

    // Skipped files become a count under the add row
    private static string SkippedCaption(IReadOnlyList<SkippedFile> skipped)
    {
        if (skipped.Count == 0)
        {
            return "";
        }

        var parts = new List<string>();
        foreach (var group in skipped.GroupBy(s => s.Reason ?? ""))
        {
            var n = group.Count();
            parts.Add(group.Key switch
            {
                "unsupported" => $"{n} skipped, not PDF or text",
                "noSpace" => $"{n} skipped, not enough free space",
                _ => $"{n} could not be read",
            });
        }

        return string.Join(" · ", parts);
    }

    private async Task LoadDocumentsAsync()
    {
        if (!_engine.Connected)
        {
            return;
        }

        if (!await EngineCall.LogAsync(_status, "guidance/documents", async () =>
            {
                var list = await _engine.ListDocumentsAsync().ConfigureAwait(true);
                GuidelinesFolder = list.Folder ?? "";
                FolderMissing = !list.Found;
                FolderInOneDrive = OneDrive.Holds(GuidelinesFolder, _oneDrive.Roots());
                var unsupported = list.Unsupported;
                Documents.Clear();
                foreach (var document in list.Documents)
                {
                    Upsert(document);
                }

                DocumentsCaption = unsupported == 0 ? ""
                    : unsupported == 1 ? "1 other file is not searched, not PDF or text"
                    : $"{unsupported} other files are not searched, not PDF or text";
            }).ConfigureAwait(true))
        {
            Documents.Clear();
        }

        OnPropertyChanged(nameof(DocumentsPresent));
        RefreshBatch();
    }

    // A row keeps its place while it works, then sorts by name below the batch
    private void Upsert(DocumentInfo document)
    {
        var row = Documents.FirstOrDefault(r => r.Id == document.Id);
        if (document.State == DocumentState.Removed)
        {
            if (row is not null)
            {
                Documents.Remove(row);
            }
        }
        else if (row is null)
        {
            row = new DocumentRow(document, RemoveDocumentCommand);
            Documents.Insert(Place(row), row);
        }
        else
        {
            var wasWorking = row.Working;
            row.Apply(document);
            if (wasWorking && !row.Working)
            {
                Documents.Remove(row);
                Documents.Insert(Place(row), row);
            }
        }

        OnPropertyChanged(nameof(DocumentsPresent));
        RefreshBatch();
    }

    // Working rows first in arrival order, then unreadable ones, then the rest, each by name
    private int Place(DocumentRow row)
    {
        static int Group(DocumentRow r) => r.Working ? 0 : r.Failed ? 1 : 2;
        var i = 0;
        while (i < Documents.Count
            && (Group(Documents[i]) < Group(row)
                || (Group(Documents[i]) == Group(row)
                    && (row.Working || string.Compare(
                        Documents[i].Name, row.Name, StringComparison.OrdinalIgnoreCase) < 0))))
        {
            i++;
        }

        return i;
    }

    // The summary follows every change. The first failure opens the list once. Work in
    // progress does not, since documents are checked again at every launch
    private void RefreshBatch()
    {
        OnPropertyChanged(nameof(DocumentsSummary));
        var attention = Documents.Any(r => r.Failed);
        if (attention && !_documentsNeededAttention && !DocumentsExpanded)
        {
            DocumentsExpanded = true;
        }

        _documentsNeededAttention = attention;
    }

    /// <summary>Remove sends the file to the Recycle Bin, so it always confirms.</summary>
    [RelayCommand]
    private async Task RemoveDocument(DocumentRow? row)
    {
        if (row is null || !_engine.Connected)
        {
            return;
        }

        if (!await _dialogs.ConfirmAsync($"Remove {row.Name}?",
                "The file is moved to the Recycle Bin and no longer searched. To keep the file, "
                + "move it out of the folder instead. Guidance already saved with a consultation "
                + "is unchanged.", "Remove").ConfigureAwait(true))
        {
            return;
        }

        await EngineCall.LogAsync(_status, "guidance/documents/remove",
            () => _engine.RemoveDocumentAsync(row.Id)).ConfigureAwait(true);
    }

    [RelayCommand]
    private async Task RemoveAllDocuments()
    {
        if (!_engine.Connected || Documents.Count == 0)
        {
            return;
        }

        if (!await _dialogs.ConfirmAsync(
                Documents.Count == 1 ? "Remove the document?" : $"Remove all {Documents.Count} documents?",
                "Every file in the folder is moved to the Recycle Bin and no longer searched. "
                + "Guidance already saved with consultations is unchanged.", "Remove all")
            .ConfigureAwait(true))
        {
            return;
        }

        await EngineCall.LogAsync(_status, "guidance/documents/removeAll", _engine.RemoveAllDocumentsAsync)
            .ConfigureAwait(true);
    }
}
