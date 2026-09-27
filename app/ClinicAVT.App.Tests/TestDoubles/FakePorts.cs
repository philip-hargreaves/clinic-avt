using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Tests.TestDoubles;

public sealed class FakeDialogService : IDialogService
{
    public bool Answer { get; set; } = true;

    public Action? OnConfirm { get; set; }

    public Action? OnEnrolment { get; set; }

    public Task<bool> ConfirmAsync(string title, string content, string primary, string cancel = "Cancel")
    {
        OnConfirm?.Invoke();
        return Task.FromResult(Answer);
    }

    /// <summary>The tick given back by a confirmation with a tick box.</summary>
    public bool Ticked { get; set; }

    /// <summary>The text of the last confirmation with a tick box.</summary>
    public string LastContent { get; private set; } = "";

    public Task<bool?> ConfirmWithOptionAsync(
        string title, string content, string tick, string primary, string cancel = "Cancel")
    {
        LastContent = content;
        OnConfirm?.Invoke();
        return Task.FromResult<bool?>(Answer ? Ticked : null);
    }

    public int BackupsRun { get; private set; }

    public Task<bool> RunBackupAsync()
    {
        BackupsRun++;
        return Task.FromResult(false);
    }

    public Task<bool> RunRestoreAsync() => Task.FromResult(false);

    /// <summary>What the import dialog hands back, null for a cancel.</summary>
    public RecordingImport? Import { get; set; }

    /// <summary>Each import dialog shown, with the dropped file it opened on.</summary>
    public List<string?> ImportsShown { get; } = [];

    public Task<RecordingImport?> RunImportAsync(string? path = null)
    {
        ImportsShown.Add(path);
        return Task.FromResult(Import);
    }

    public Task<bool> RunEnrolmentAsync()
    {
        OnEnrolment?.Invoke();
        return Task.FromResult(true);
    }

    public Task ShowReflectionAsync(string sessionId, string startedAt) => Task.CompletedTask;
}

public sealed class FakeFilePicker : IFilePicker
{
    public string? SavePath { get; set; }

    public IReadOnlyList<string> Files { get; set; } = [];

    public List<string> SuggestedNames { get; } = [];

    public Task<string?> PickSaveAsync(string suggestedName, string typeLabel, string extension)
    {
        SuggestedNames.Add(suggestedName);
        return Task.FromResult(SavePath);
    }

    /// <summary>The file an open picker returns, null for a cancel.</summary>
    public string? OpenPath { get; set; }

    public Task<string?> PickFileAsync(IReadOnlyList<string> extensions) => Task.FromResult(OpenPath);

    public Task<IReadOnlyList<string>> PickFilesAsync(IReadOnlyList<string> extensions) =>
        Task.FromResult(Files);
}

public sealed class FakeLauncher : ILauncher
{
    public List<string> Files { get; } = [];

    public Task<bool> OpenLinkAsync(string link) => Task.FromResult(true);

    public Task OpenFileAsync(string path)
    {
        Files.Add(path);
        return Task.CompletedTask;
    }

    public void RevealFolder(string path)
    {
    }
}

public sealed class FakeClipboard : IClipboard
{
    public List<string> Copied { get; } = [];

    public Task<bool> CopyAsync(string text)
    {
        Copied.Add(text);
        return Task.FromResult(true);
    }

    /// <summary>Secrets copied, kept apart from ordinary copies.</summary>
    public List<string> Secrets { get; } = [];

    public Task<bool> CopySecretAsync(string text)
    {
        Secrets.Add(text);
        return Task.FromResult(true);
    }
}

public sealed class FakeThemeService : IThemeService
{
    public List<string> Applied { get; } = [];

    public void Apply(string theme) => Applied.Add(theme);
}
