using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Guidance;

/// <summary>
/// The page view beside the note. It shows a page of an added document, opens on the cited
/// passage with its lines marked, and can be turned from there.
/// </summary>
public sealed partial class PageViewModel(
    IGuidanceApi engine, ILauncher launcher, IClipboard clipboard, IStatusLine status, TimeProvider time)
    : ObservableObject, IDocumentPages
{
    private static readonly TimeSpan SlowAfter = TimeSpan.FromMilliseconds(1500);

    private GuidanceRecommendation? _shown;
    private int _page;
    private int _pages;
    private int _load;

    [ObservableProperty]
    public partial bool Visible { get; private set; }

    [ObservableProperty]
    public partial string DocumentName { get; private set; } = "";

    /// <summary>"Page 2 of 5".</summary>
    [ObservableProperty]
    public partial string PageLabel { get; private set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SheetVisible), nameof(PlaceholderVisible))]
    public partial bool Loading { get; private set; }

    /// <summary>Set after 1.5 s without the page, for a still-loading caption.</summary>
    [ObservableProperty]
    public partial bool Slow { get; private set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SheetVisible), nameof(PlaceholderVisible))]
    public partial bool Failed { get; private set; }

    [ObservableProperty]
    public partial string ImagePath { get; private set; } = "";

    [ObservableProperty]
    public partial double Width { get; private set; } = 595;

    [ObservableProperty]
    public partial double Height { get; private set; } = 842;

    /// <summary>True once the page is turned away from the passage.</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(BackToPassageCommand))]
    public partial bool OffPassage { get; private set; }

    /// <summary>The page's marks as one region, for the view to scroll to.</summary>
    [ObservableProperty]
    public partial PageBox? Focus { get; private set; }

    /// <summary>Raised on every show, since a page already open reports no change.</summary>
    public event Action? Shown;

    public ObservableCollection<PageBox> Boxes { get; } = [];

    public bool SheetVisible => !Loading && !Failed;

    public bool PlaceholderVisible => Loading || Failed;

    public string ImageName => _shown is null ? ""
        : Boxes.Count == 0 ? $"Page {_page + 1} of {DocumentName}"
        : $"Page {_page + 1} of {DocumentName}, "
          + $"{(_shown.Number.Length > 0 ? _shown.Number : "the passage")} highlighted";

    public bool CanGoBack => _page > 0;

    public bool CanGoForward => _page < _pages - 1;

    [RelayCommand]
    private void Close() => Hide();

    [RelayCommand]
    private Task TryAgain() => LoadAsync();

    [RelayCommand(CanExecute = nameof(CanGoBack))]
    private Task PreviousPage()
    {
        _page--;
        return LoadAsync();
    }

    [RelayCommand(CanExecute = nameof(CanGoForward))]
    private Task NextPage()
    {
        _page++;
        return LoadAsync();
    }

    [RelayCommand(CanExecute = nameof(OffPassage))]
    private Task BackToPassage()
    {
        _page = _shown?.Page ?? _page;
        return LoadAsync();
    }

    [RelayCommand]
    private Task OpenDocument() => _shown is null ? Task.CompletedTask : OpenAsync(_shown);

    [RelayCommand]
    private Task CopyCitation() => clipboard.CopyAsync(status, _shown?.Citation ?? "", "Citation");

    /// <summary>Opens on the passage's page and asks the engine to draw it.</summary>
    public async Task ShowAsync(GuidanceRecommendation found)
    {
        _shown = found;
        _page = found.Page;
        _pages = found.Pages;
        DocumentName = found.Title;
        Visible = true;
        Shown?.Invoke();
        await LoadAsync().ConfigureAwait(true);
    }

    /// <summary>A card's Open, which shows the decrypted copy in the PDF viewer.</summary>
    public async Task OpenAsync(GuidanceRecommendation found)
    {
        try
        {
            var path = await engine.OpenDocumentAsync(found.Document).ConfigureAwait(true);
            if (path.Length > 0)
            {
                await launcher.OpenFileAsync(path).ConfigureAwait(true);
            }
        }
        catch (Exception)
        {
            status.Append("The document could not be opened");
        }
    }

    /// <summary>Closes the view when the passage it shows is missing from the cards.</summary>
    public void KeepOnlyFor(IEnumerable<GuidanceCard> cards)
    {
        if (Visible && _shown is not null
            && !cards.Any(card => card.Recommendations.Any(r => r.ChunkId == _shown.ChunkId)))
        {
            Hide();
        }
    }

    public void Hide()
    {
        Visible = false;
        _load++;
    }

    // A guidance/page reply carries the bitmap, its size and the passage's boxes. The boxes
    // come whole each time, so a page without any is a page the passage is not on
    private void Apply(GuidancePage reply)
    {
        Width = reply.Width;
        Height = reply.Height;
        ImagePath = reply.Path ?? "";
        if (reply.Pages > 0)
        {
            _pages = reply.Pages;
        }

        Boxes.Clear();
        var onPage = reply.Boxes.Where(b => b.Page == _page).ToList();
        foreach (var box in onPage)
        {
            Boxes.Add(new PageBox(box.Left * Width, box.Top * Height,
                (box.Right - box.Left) * Width, (box.Bottom - box.Top) * Height));
        }

        Focus = onPage.Count == 0 ? null : new PageBox(
            onPage.Min(b => b.Left) * Width, onPage.Min(b => b.Top) * Height,
            (onPage.Max(b => b.Right) - onPage.Min(b => b.Left)) * Width,
            (onPage.Max(b => b.Bottom) - onPage.Min(b => b.Top)) * Height);

        UpdatePageLabel();
        Loading = false;
        Slow = false;
        OnPropertyChanged(nameof(ImageName));
    }

    private void UpdatePageLabel() =>
        PageLabel = _pages > 0 ? $"Page {_page + 1} of {_pages}" : $"Page {_page + 1}";

    private async Task LoadAsync()
    {
        if (_shown is null)
        {
            return;
        }

        var load = ++_load;
        Loading = true;
        Slow = false;
        Failed = false;
        Boxes.Clear();
        Focus = null;
        ImagePath = "";
        UpdatePageLabel();
        OffPassage = _page != _shown.Page;
        PreviousPageCommand.NotifyCanExecuteChanged();
        NextPageCommand.NotifyCanExecuteChanged();
        _ = MarkSlowAsync(load);
        try
        {
            var reply = await engine.PageAsync(_shown.Document, _page, _shown.ChunkId)
                .ConfigureAwait(true);
            if (load == _load)
            {
                Apply(reply);
            }
        }
        catch (Exception e)
        {
            if (load == _load)
            {
                Failed = true;
                Loading = false;
                Slow = false;
                status.Append($"The page could not be shown: {e.Message}");
            }
        }
    }

    private async Task MarkSlowAsync(int load)
    {
        await Task.Delay(SlowAfter, time).ConfigureAwait(true);
        if (load == _load && Loading)
        {
            Slow = true;
        }
    }
}
