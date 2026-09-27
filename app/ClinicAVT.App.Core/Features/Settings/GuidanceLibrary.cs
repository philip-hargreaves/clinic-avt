using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>
/// What the guidance search covers. That is the installed corpora as the engine reports them
/// and the clinician's own documents in the guidelines folder.
/// </summary>
public sealed partial class GuidanceLibrary : ObservableObject
{
    private readonly AppPreferences? _preferences;
    private readonly IEngineApi? _client;
    private readonly StatusBarViewModel? _status;
    private readonly IDialogService? _dialogs;
    private readonly IFilePicker? _picker;
    private readonly ILauncher? _launcher;
    private readonly bool _initialising;
    private bool _documentsNeededAttention;

    public GuidanceLibrary(
        AppPreferences? preferences, IEngineApi? client, StatusBarViewModel? status,
        IDialogService? dialogs, IFilePicker? picker, ILauncher? launcher)
    {
        _preferences = preferences;
        _client = client;
        _status = status;
        _dialogs = dialogs;
        _picker = picker;
        _launcher = launcher;
        // Restoring saved values is not the clinician changing them
        _initialising = true;
        IncludeResearchGuidance = preferences?.IncludeResearchGuidance ?? false;
        _initialising = false;
    }

    /// <summary>
    /// Reloads both lists from the engine when it connects or reloads its corpora.
    /// </summary>
    public void Connected()
    {
        _ = LoadGuidanceCorporaAsync();
        _ = LoadDocumentsAsync();
    }

    /// <summary>A document changed, or an ingest reported progress.</summary>
    public void Apply(EngineNotification notification)
    {
        switch (notification)
        {
            case GuidanceModelChanged:
                Connected();
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

    /// <summary>The corpora the engine has, refused ones included.</summary>
    public ObservableCollection<CorpusRow> GuidanceCorpora { get; } = [];

    /// <summary>Why nothing is listed, empty when corpora are shown.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(GuidanceCaptionVisible), nameof(GuidanceInstalledVisible))]
    public partial string GuidanceCaption { get; set; } = "";

    public bool GuidanceCaptionVisible => GuidanceCaption.Length > 0;

    /// <summary>Hidden when nothing is installed, so no empty card shows.</summary>
    public bool GuidanceInstalledVisible => GuidanceCorpora.Count > 0 || GuidanceCaption.Length > 0;

    private async Task LoadGuidanceCorporaAsync()
    {
        if (!_client.IsConnected())
        {
            return;
        }

        if (!await EngineCall.LogAsync(_status, "guidance/corpora",
                async () => ApplyGuidanceCorpora(await _client.GuidanceCorporaAsync().ConfigureAwait(true)))
            .ConfigureAwait(true))
        {
            GuidanceCorpora.Clear();
            GuidanceCaption = "Unavailable";
        }
    }

    private void ApplyGuidanceCorpora(CorporaStatus reply)
    {
        GuidanceCorpora.Clear();
        foreach (var corpus in reply.Corpora)
        {
            GuidanceCorpora.Add(RowFrom(corpus));
        }

        var detail = reply.Detail ?? "";
        GuidanceCaption = reply.State switch
        {
            "loading" => "Loading",
            "unavailable" => detail.Length > 0 ? $"Unavailable: {detail}" : "Unavailable",
            _ => "",
        };
        OnPropertyChanged(nameof(GuidanceInstalledVisible));
    }

    // A refused corpus keeps its place, so unused guidance stays visible
    private static CorpusRow RowFrom(CorpusInfo corpus)
    {
        var refused = corpus.Unavailable ?? "";
        if (refused.Length > 0)
        {
            return new CorpusRow(corpus.Id, $"Not used: {refused}", "", true);
        }

        // The licence is not shown because the attribution line is what it asks for
        var parts = new List<string>();
        if (corpus.Chunks > 0)
        {
            parts.Add($"{corpus.Chunks:N0} passages");
        }

        var built = Words.ShortDate(corpus.BuiltAt ?? "");
        if (built.Length > 0)
        {
            parts.Add(built);
        }

        return new CorpusRow(corpus.Name, string.Join(" · ", parts), corpus.Attribution ?? "", false);
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

    [RelayCommand]
    private void OpenFolder()
    {
        if (GuidelinesFolder.Length > 0)
        {
            _launcher?.RevealFolder(GuidelinesFolder);
        }
    }

    [RelayCommand]
    private async Task AddDocuments()
    {
        if (_picker is null)
        {
            return;
        }

        var paths = await _picker.PickFilesAsync([".pdf", ".txt", ".md"]).ConfigureAwait(true);
        if (paths.Count > 0)
        {
            await AddPathsAsync(paths).ConfigureAwait(true);
        }
    }

    private async Task AddPathsAsync(IReadOnlyList<string> paths)
    {
        if (!_client.IsConnected())
        {
            return;
        }

        if (!await EngineCall.LogAsync(_status, "guidance/documents/add", async () =>
            {
                var added = await _client.AddDocumentsAsync(paths).ConfigureAwait(true);
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
        if (!_client.IsConnected())
        {
            return;
        }

        if (!await EngineCall.LogAsync(_status, "guidance/documents", async () =>
            {
                var list = await _client.ListDocumentsAsync().ConfigureAwait(true);
                GuidelinesFolder = list.Folder ?? "";
                FolderMissing = !list.Found;
                FolderInOneDrive = InOneDrive(GuidelinesFolder, OneDriveRoots());
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
        if (document.State == "removed")
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

    private static readonly string[] OneDriveVariables =
        ["OneDrive", "OneDriveConsumer", "OneDriveCommercial"];

    private static IEnumerable<string> OneDriveRoots() =>
        OneDriveVariables.Select(Environment.GetEnvironmentVariable)
            .Where(root => !string.IsNullOrEmpty(root))
            .Select(root => root!);

    // A folder under a OneDrive root syncs to the cloud, which the caption states
    public static bool InOneDrive(string folder, IEnumerable<string> roots) =>
        folder.Length > 0
        && roots.Any(root => folder.StartsWith(root, StringComparison.OrdinalIgnoreCase));

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
        if (row is null || !_client.IsConnected())
        {
            return;
        }

        if (_dialogs is not null && !await _dialogs.ConfirmAsync($"Remove {row.Name}?",
                "The file is moved to the Recycle Bin and no longer searched. To keep the file, "
                + "move it out of the folder instead. Guidance already saved with a consultation "
                + "is unchanged.", "Remove").ConfigureAwait(true))
        {
            return;
        }

        await EngineCall.LogAsync(_status, "guidance/documents/remove",
            () => _client.RemoveDocumentAsync(row.Id)).ConfigureAwait(true);
    }

    [RelayCommand]
    private async Task RemoveAllDocuments()
    {
        if (!_client.IsConnected() || Documents.Count == 0)
        {
            return;
        }

        if (_dialogs is not null && !await _dialogs.ConfirmAsync(
                Documents.Count == 1 ? "Remove the document?" : $"Remove all {Documents.Count} documents?",
                "Every file in the folder is moved to the Recycle Bin and no longer searched. "
                + "Guidance already saved with consultations is unchanged.", "Remove all")
            .ConfigureAwait(true))
        {
            return;
        }

        await EngineCall.LogAsync(_status, "guidance/documents/removeAll", _client.RemoveAllDocumentsAsync)
            .ConfigureAwait(true);
    }

    /// <summary>
    /// Shows the switch for the local research corpus, the NICE demo, in a debug build only.
    /// </summary>
    public bool ResearchToggleVisible { get; } = BuildFlags.Debug;

    [ObservableProperty]
    public partial bool IncludeResearchGuidance { get; set; }

    partial void OnIncludeResearchGuidanceChanged(bool value)
    {
        if (_initialising)
        {
            return;
        }

        _preferences.Update(p => p.IncludeResearchGuidance = value);
        _ = ApplyResearchAsync(value);
    }

    // The engine reloads its corpora live and announces the change, so the
    // list follows without a restart
    private async Task ApplyResearchAsync(bool include)
    {
        if (!_client.IsConnected())
        {
            return;
        }

        await EngineCall.LogAsync(_status, "guidance/research",
            () => _client.SetResearchGuidanceAsync(include)).ConfigureAwait(true);
    }
}
