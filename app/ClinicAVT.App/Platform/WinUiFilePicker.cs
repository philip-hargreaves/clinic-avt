using ClinicAVT.App.Core.Ports;
using Windows.Storage.Pickers;

namespace ClinicAVT.App.Platform;

public sealed class WinUiFilePicker(WindowAccessor window) : IFilePicker
{
    public async Task<string?> PickSaveAsync(string suggestedName, string typeLabel, string extension)
    {
        var picker = new FileSavePicker
        {
            SuggestedFileName = suggestedName,
            SuggestedStartLocation = PickerLocationId.DocumentsLibrary,
        };
        picker.FileTypeChoices.Add(typeLabel, [extension]);
        if (!window.BindToWindow(picker))
        {
            return null;
        }

        var file = await picker.PickSaveFileAsync();
        return file?.Path;
    }

    public async Task<string?> PickFileAsync(IReadOnlyList<string> extensions)
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.Downloads };
        foreach (var extension in extensions)
        {
            picker.FileTypeFilter.Add(extension);
        }

        if (!window.BindToWindow(picker))
        {
            return null;
        }

        var file = await picker.PickSingleFileAsync();
        return file?.Path;
    }

    public async Task<IReadOnlyList<string>> PickFilesAsync(IReadOnlyList<string> extensions)
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary };
        foreach (var extension in extensions)
        {
            picker.FileTypeFilter.Add(extension);
        }

        if (!window.BindToWindow(picker))
        {
            return [];
        }

        var files = await picker.PickMultipleFilesAsync();
        return files.Select(f => f.Path).ToArray();
    }
}
