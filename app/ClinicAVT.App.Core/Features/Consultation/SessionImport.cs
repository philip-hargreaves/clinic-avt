using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// A recording from a file. The engine decodes and stores it, then finalises it as a stop
/// does, so the same stages and the note follow.
/// </summary>
public sealed partial class SessionImport(SessionRecorder recorder, IRecordingApi engine, IStatusLine status)
    : ObservableObject
{
    /// <summary>True from an import's request until the engine has sealed it or it ends.</summary>
    [ObservableProperty]
    public partial bool Importing { get; private set; }

    /// <summary>How far the import has got, in words and one percentage, null until the engine says.</summary>
    [ObservableProperty]
    public partial string? ImportLine { get; private set; }

    /// <summary>An import began, with when the consultation took place.</summary>
    public event Action<RecordingImport>? ImportStarted;

    public async Task ImportRecordingAsync(RecordingImport import)
    {
        if (recorder.State != SessionState.Idle)
        {
            return;
        }

        recorder.BeginImport(import.Seconds);
        ImportStarted?.Invoke(import);
        ImportLine = null;
        Importing = true;
        try
        {
            await recorder.FinaliseAsync("session/import", "Could not import the recording",
                () => engine.ImportRecordingAsync(import.Path, import.StartedAt, recorder.Retain))
                .ConfigureAwait(true);
        }
        finally
        {
            Importing = false;
        }
    }

    /// <summary>The import's stage, with the one percentage across all of them until it finalises.</summary>
    public void OnImportProgress(ImportProgress progress)
    {
        if (!Importing || recorder.Phase >= FinalisePhase.Note)
        {
            return;
        }

        var line = progress.Stage switch
        {
            ImportStage.Reading or ImportStage.Speech => $"Preparing · {progress.Percent}%",
            ImportStage.Transcribing => $"Transcribing · {progress.Percent}%",
            _ => "Finalising",
        };
        if (line == ImportLine)
        {
            return;
        }

        ImportLine = line;
        status.Show(line, busy: true);
    }

    /// <summary>Stops an import. The engine erases what it began and the page goes back to idle.</summary>
    public async Task CancelImportAsync()
    {
        if (Importing)
        {
            await EngineCall.TryAsync(status, "session/cancel", () => engine.CancelSessionAsync())
                .ConfigureAwait(true);
        }
    }
}
