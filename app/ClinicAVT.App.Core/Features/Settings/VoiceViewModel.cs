using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Settings;

/// <summary>
/// The clinician's voiceprint. It is learned from consultations, or set up by reading a
/// passage and then refined by consultations. There is only ever one.
/// </summary>
public sealed partial class VoiceViewModel : ObservableObject
{
    // Below this the automatic print is still settling, and the copy says so
    private const int LearnedAfterSessions = 5;

    private readonly IEngineApi _engine;
    private readonly IDialogService _dialogs;
    private readonly ISessionState? _session;
    private readonly StatusBarViewModel? _status;
    private readonly TimeProvider _clock;

    public VoiceViewModel(IEngineApi engine, IDialogService dialogs, ISessionState? session = null,
        StatusBarViewModel? status = null, TimeProvider? clock = null)
    {
        _engine = engine;
        _dialogs = dialogs;
        _session = session;
        _status = status;
        _clock = clock ?? TimeProvider.System;
        _engine.ConnectedChanged += connected =>
        {
            SetUpVoiceCommand.NotifyCanExecuteChanged();
            ForgetVoiceCommand.NotifyCanExecuteChanged();
            if (connected)
            {
                _ = RefreshAsync();
            }
        };
    }

    /// <summary>"none", "accrued" or "enrolled", as the engine reports it.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Headline), nameof(HasVoice), nameof(SetUpLabel))]
    [NotifyCanExecuteChangedFor(nameof(ForgetVoiceCommand))]
    public partial string Origin { get; private set; } = "none";

    /// <summary>Consultations that refined the print since it began.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Headline))]
    public partial int Sessions { get; private set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Headline))]
    public partial DateTimeOffset? EnrolledAt { get; private set; }

    /// <summary>True while an enrolment or a clear is in flight.</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(SetUpVoiceCommand), nameof(ForgetVoiceCommand))]
    public partial bool Busy { get; private set; }

    public bool HasVoice => Origin != "none";

    public string SetUpLabel => HasVoice ? "Redo" : "Set up";

    public string Headline => Origin switch
    {
        "enrolled" => EnrolledDescription(),
        "accrued" when Sessions < LearnedAfterSessions =>
            $"Learning automatically, {Words.Count(Sessions, "consultation")} so far",
        "accrued" => $"Learned automatically from {Words.Count(Sessions, "consultation")}",
        _ => "Tells you apart from the patient. Learned automatically, or set up now.",
    };

    public async Task RefreshAsync()
    {
        if (!_engine.Connected)
        {
            return;
        }

        // An unreachable engine leaves the last known state on screen
        await EngineCall.LogAsync(_status, "anchor/status", async () =>
        {
            var status = await _engine.AnchorStatusAsync().ConfigureAwait(true);
            Origin = status.Origin;
            Sessions = status.Sessions;
            EnrolledAt = status.EnrolledAt is { } at ? DateTimeOffset.FromUnixTimeSeconds(at) : null;
        }).ConfigureAwait(true);
    }

    [RelayCommand(CanExecute = nameof(CanSetUp))]
    private async Task SetUpVoice()
    {
        if (ConsultationGuard.Blocks(_session, _status, "voice enrolment"))
        {
            return;
        }

        Busy = true;
        try
        {
            var made = await _dialogs.RunEnrolmentAsync().ConfigureAwait(true);
            await RefreshAsync().ConfigureAwait(true);
            if (made)
            {
                _status?.Append("voice enrolment complete");
            }
        }
        finally
        {
            Busy = false;
        }
    }

    private bool CanSetUp() => _engine.Connected && !Busy;

    [RelayCommand(CanExecute = nameof(CanForget))]
    private async Task ForgetVoice()
    {
        if (ConsultationGuard.Blocks(_session, _status, "forgetting voice enrolment"))
        {
            return;
        }

        // Forgetting cannot be undone, so it is asked once
        if (!await _dialogs.ConfirmAsync("Forget voice enrolment?",
                "It will be learned again from your next consultation.", "Forget")
            .ConfigureAwait(true))
        {
            return;
        }

        Busy = true;
        try
        {
            if (await EngineCall.ReportAsync(_status, "could not forget voice enrolment",
                    _engine.ClearAnchorAsync).ConfigureAwait(true))
            {
                await RefreshAsync().ConfigureAwait(true);
                _status?.Append("voice enrolment forgotten");
            }
        }
        finally
        {
            Busy = false;
        }
    }

    private bool CanForget() => _engine.Connected && !Busy && HasVoice;

    private string EnrolledDescription()
    {
        var date = EnrolledAt?.ToLocalTime() is { } when
            ? $"Set up on {Words.Day(when, _clock.GetLocalNow().Year)}"
            : "Set up";
        return Sessions > 0
            ? $"{date}, refined automatically by {Words.Count(Sessions, "consultation")} since"
            : date;
    }
}
