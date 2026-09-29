using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>
/// One voice-enrolment reading. Start, read the passage, Finish, then the engine accepts or
/// rejects it. The dialog closes once a reading succeeds.
/// </summary>
public sealed partial class EnrolmentViewModel : ObservableObject, IDisposable
{
    /// <summary>Hidden safety cap. Readings normally end with Finish.</summary>
    public const double DefaultSeconds = 120;

    /// <summary>What the engine needs before it will make a print.</summary>
    public const double NeededSpeechSeconds = 20;

    /// <summary>Where the bar fills. Five seconds past what the engine needs, so the reading
    /// is never rushed.</summary>
    public const double TargetSpeechSeconds = NeededSpeechSeconds + 5;

    /// <summary>About 30 s at a natural pace, over the 20 s of speech the engine needs.</summary>
    public const string Passage =
        "I am reading this so ClinicAVT learns my voice and can tell me apart from my patients. "
        + "Good morning, thanks for coming in. Have you had any chest pain, shortness of breath "
        + "or dizziness in the last two weeks? Are you taking any regular medication? I will "
        + "check your blood pressure and listen to your chest, and then we can talk about what "
        + "happens next. Do you have any questions before we start?";

    private readonly IEnrolmentApi _engine;
    private readonly ILogger<EnrolmentViewModel> _logger;
    private readonly string _micId;
    private readonly IDisposable _notifications;
    private readonly TaskCompletionSource<bool> _outcome =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public EnrolmentViewModel(
        IEnrolmentApi engine, IEngineEvents events, IMicrophoneChoice microphone,
        ILogger<EnrolmentViewModel> logger)
    {
        _engine = engine;
        _micId = microphone.MicId;
        _logger = logger;
        _notifications = events.Subscribe<EngineNotification>(OnNotification);
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(StatusLine), nameof(PrimaryText), nameof(CloseText),
        nameof(Recording), nameof(KeepsOpen))]
    public partial EnrolmentState State { get; private set; } = EnrolmentState.Ready;

    /// <summary>Microphone level, 0 to 1, for the ring.</summary>
    [ObservableProperty]
    public partial double Level { get; private set; }

    /// <summary>Clear speech captured so far, in seconds.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Progress), nameof(StatusLine), nameof(EnoughCaptured))]
    public partial double Speech { get; private set; }

    /// <summary>The engine's reason when the reading was not enough.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(StatusLine))]
    public partial string Detail { get; private set; } = "";

    public string PassageText { get; } = Passage;

    public bool Recording => State == EnrolmentState.Recording;

    public double Progress => Math.Clamp(Speech / TargetSpeechSeconds, 0, 1);

    public bool EnoughCaptured => Speech >= TargetSpeechSeconds;

    public string StatusLine => State switch
    {
        EnrolmentState.Ready => "Press Start, then read the passage aloud.",
        EnrolmentState.Recording when EnoughCaptured =>
            "Enough captured. Finish whenever you reach the end.",
        EnrolmentState.Recording => "Listening. Read to the end, then press Finish.",
        EnrolmentState.Succeeded => "Voice enrolment complete.",
        _ => Detail.Length > 0 ? $"That did not work: {Detail}." : "That did not work.",
    };

    public string PrimaryText => State switch
    {
        EnrolmentState.Ready => "Start",
        EnrolmentState.Recording => "Finish",
        EnrolmentState.Succeeded => "Done",
        _ => "Try again",
    };

    public string CloseText => State switch
    {
        EnrolmentState.Succeeded => "",
        EnrolmentState.Failed => "Close",
        _ => "Cancel",
    };

    /// <summary>Start, Finish and Try again keep the dialog open. Only Done closes it.</summary>
    public bool KeepsOpen => State != EnrolmentState.Succeeded;

    /// <summary>True once a print was made. False on cancel, failure or dismissal.</summary>
    public Task<bool> Outcome => _outcome.Task;

    private async Task Start()
    {
        if (!_engine.Connected)
        {
            Fail("recording is not available yet");
            return;
        }

        State = EnrolmentState.Recording;
        Speech = 0;
        Detail = "";
        try
        {
            await _engine.StartEnrolmentAsync(DefaultSeconds, _micId).ConfigureAwait(true);
        }
        catch (Exception e)
        {
            Fail(e.Message);
        }
    }

    private async Task Cancel()
    {
        try
        {
            await _engine.CancelEnrolmentAsync().ConfigureAwait(true);
        }
        catch (Exception e)
        {
            // The engine will report the outcome, or the dialog is closing anyway
            _logger.StepFailed("anchor/enrol/cancel", e.Message);
        }
    }

    private async Task Finish()
    {
        try
        {
            await _engine.FinishEnrolmentAsync().ConfigureAwait(true);
        }
        catch (Exception e)
        {
            Fail(e.Message);
        }
    }

    /// <summary>Start, then Finish, then Try again. Done only closes the dialog.</summary>
    [RelayCommand]
    private Task Primary() => State switch
    {
        EnrolmentState.Recording => Finish(),
        EnrolmentState.Succeeded => Task.CompletedTask,
        _ => Start(),
    };

    /// <summary>The dialog was dismissed. A reading in flight is cancelled.</summary>
    public void Dismiss()
    {
        if (State == EnrolmentState.Recording)
        {
            _ = Cancel();
        }

        _outcome.TrySetResult(State == EnrolmentState.Succeeded);
    }

    public void Dispose()
    {
        _notifications.Dispose();
        _outcome.TrySetResult(State == EnrolmentState.Succeeded);
    }

    private void OnNotification(EngineNotification notification)
    {
        switch (notification)
        {
            case EnrolmentProgress progress:
                Apply(progress);
                break;
            case EnrolmentDone done:
                Apply(done);
                break;
            default:
                break;
        }
    }

    private void Apply(EnrolmentProgress progress)
    {
        if (State == EnrolmentState.Recording)
        {
            Level = progress.Level;
            Speech = progress.Speech;
        }
    }

    private void Apply(EnrolmentDone done)
    {
        Level = 0;
        if (done.Ok)
        {
            State = EnrolmentState.Succeeded;
            _outcome.TrySetResult(true);
        }
        else
        {
            Fail(done.Detail ?? "");
        }
    }

    private void Fail(string detail)
    {
        Detail = detail;
        State = EnrolmentState.Failed;
    }
}
