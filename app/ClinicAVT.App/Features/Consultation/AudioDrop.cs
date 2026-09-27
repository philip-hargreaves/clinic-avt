using Microsoft.UI.Xaml;
using ClinicAVT.App.Core.Features.Consultation;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;

namespace ClinicAVT.App.Features.Consultation;

/// <summary>
/// Takes the first recording dragged onto an element and shows the highlight while one is over
/// it. Enter and leave bubble from every child, so a count tracks when the drag has left.
/// </summary>
internal sealed class AudioDrop
{
    private readonly UIElement _highlight;
    private readonly Func<bool> _allowed;
    private readonly Func<string, Task> _dropped;
    private int _inside;
    private string? _path;

    private AudioDrop(UIElement highlight, Func<bool> allowed, Func<string, Task> dropped)
    {
        _highlight = highlight;
        _allowed = allowed;
        _dropped = dropped;
    }

    private bool Over => _inside > 0 && _path is not null;

    public static void Attach(
        UIElement target, UIElement highlight, Func<bool> allowed, Func<string, Task> dropped)
    {
        var drop = new AudioDrop(highlight, allowed, dropped);
        target.AllowDrop = true;
        target.DragEnter += drop.OnDragEnter;
        target.DragOver += drop.OnDragOver;
        target.DragLeave += drop.OnDragLeave;
        target.Drop += drop.OnDrop;
    }

    private async void OnDragEnter(object sender, DragEventArgs e)
    {
        if (_inside++ == 0)
        {
            var deferral = e.GetDeferral();
            try
            {
                _path = _allowed() ? await FirstRecordingAsync(e.DataView) : null;
            }
            finally
            {
                deferral.Complete();
            }
        }

        Accept(e);
        Highlight();
    }

    private void OnDragOver(object sender, DragEventArgs e) => Accept(e);

    private void OnDragLeave(object sender, DragEventArgs e)
    {
        _inside = Math.Max(0, _inside - 1);
        Highlight();
    }

    private async void OnDrop(object sender, DragEventArgs e)
    {
        var path = Over ? _path : null;
        _inside = 0;
        _path = null;
        Highlight();
        // The page may have moved on while the drag was over it
        if (path is not null && _allowed())
        {
            await _dropped(path);
        }
    }

    private void Accept(DragEventArgs e)
    {
        e.AcceptedOperation = Over ? DataPackageOperation.Copy : DataPackageOperation.None;
        if (Over)
        {
            // The target says what a drop does, so the cursor needs no caption
            e.DragUIOverride.IsCaptionVisible = false;
        }
    }

    private void Highlight() => _highlight.Opacity = Over ? 1 : 0;

    private static async Task<string?> FirstRecordingAsync(DataPackageView data)
    {
        if (!data.Contains(StandardDataFormats.StorageItems))
        {
            return null;
        }

        try
        {
            var items = await data.GetStorageItemsAsync();
            return items.OfType<StorageFile>().Select(f => f.Path)
                .FirstOrDefault(p => p.Length > 0 && ImportRecordingViewModel.IsAudio(p));
        }
        catch (Exception)
        {
            // Some sources refuse their storage items
            return null;
        }
    }
}
