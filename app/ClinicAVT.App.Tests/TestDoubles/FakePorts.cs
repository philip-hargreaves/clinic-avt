using ClinicAVT.App.Core.Features.Appraisal;
using ClinicAVT.App.Core.Features.Backup;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Examples;
using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Core.Metrics;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;

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

    /// <summary>Every dialog shown, by its view model.</summary>
    public List<object> Shown { get; } = [];

    /// <summary>
    /// Runs while a dialog is open, as the clinician would use it. The answer is whether the
    /// primary button closed it, false when unset.
    /// </summary>
    public Func<object, Task<bool>>? OnShow { get; set; }

    public int BackupsRun => Shown.Count(d => d is BackupViewModel);

    public int ExportsRun => Shown.Count(d => d is ExportReflectionsViewModel);

    /// <summary>The recording the import dialog hands back, null for a cancel.</summary>
    public RecordingImport? Import { get; set; }

    /// <summary>Each import dialog shown, with the dropped file it opened on.</summary>
    public List<string?> ImportsShown { get; } = [];

    public async Task<bool> ShowAsync(object viewModel)
    {
        Shown.Add(viewModel);
        switch (viewModel)
        {
            case ImportRecordingViewModel import:
                ImportsShown.Add(import.Path.Length > 0 ? import.Path : null);
                if (Import is null)
                {
                    return false;
                }

                if (import.Path != Import.Path)
                {
                    await import.UseFileAsync(Import.Path);
                }

                return import.Result is not null;
            case EnrolmentViewModel enrolment:
                OnEnrolment?.Invoke();
                enrolment.Dismiss();
                return false;
        }

        return OnShow is not null && await OnShow(viewModel);
    }
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

    public List<string> Folders { get; } = [];

    public void RevealFolder(string path) => Folders.Add(path);
}

public sealed class FakeClipboard : IClipboard
{
    public List<string> Copied { get; } = [];

    public Task<bool> CopyAsync(string text)
    {
        Copied.Add(text);
        return Task.FromResult(true);
    }
}

public sealed class FakeThemeService : IThemeService
{
    public List<AppTheme> Applied { get; } = [];

    public void Apply(AppTheme theme) => Applied.Add(theme);
}

public sealed class FakeTextFiles : ITextFiles
{
    /// <summary>Every file written, by path.</summary>
    public Dictionary<string, string> Written { get; } = [];

    /// <summary>Thrown by the next write, once.</summary>
    public Exception? FailNext { get; set; }

    public Task WriteAsync(string path, string text)
    {
        if (FailNext is { } failure)
        {
            FailNext = null;
            return Task.FromException(failure);
        }

        Written[path] = text;
        return Task.CompletedTask;
    }
}

public sealed class FakeMetricsLog : IMetricsLog
{
    public List<string> Lines { get; } = [];

    public void Append(string line) => Lines.Add(line);

    public IReadOnlyList<string>? ReadAll() => Lines.Count == 0 ? null : Lines;
}

public sealed class FakeOneDriveFolders : IOneDriveFolders
{
    public string? Work { get; set; }

    public string? Personal { get; set; }

    public string? Primary { get; set; }
}

public sealed class FakeExampleLibrary : IExampleLibrary
{
    public List<ExampleCase> Cases { get; } = [];

    public List<ExampleRecording> Recordings { get; } = [];

    public IReadOnlyList<ExampleCase> LoadCases() => Cases;

    public IReadOnlyList<ExampleRecording> LoadRecordings() => Recordings;
}

public sealed class FakeProcessMetrics : IProcessMetrics
{
    /// <summary>What CommittedGb reports. 0 means the reader found nothing.</summary>
    public double Committed { get; set; }

    public long? PeakWorkingSetMb(int pid) => null;

    public long? PeakCommitMb(int pid) => null;

    public long? PeakWorkingSetMbOf(string processName) => null;

    public double CommittedGb(params string[] processNames) => Committed;
}

public sealed class FixedPowerState : IPowerStateReader
{
    public PowerState Read() => PowerState.Unknown;
}

public sealed class FixedMachine : IMachineInfoProvider
{
    public MachineInfo Describe() => new("cpu", 32, "os", [], null);

    public string MachineName => "TESTPC";
}

public sealed class FixedAppInfo : IAppInfo
{
    public string Version => "0.0.0";
}

public sealed class NoCredits : ICreditsSource
{
    public IReadOnlyList<CreditMark> LoadMarks() => [];
}
