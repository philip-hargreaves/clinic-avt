using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Features.Settings;

public class VoiceViewModelTest
{
    [Fact]
    public async Task SettingUpRunsTheDialogRereadsTheEngineAndTheHeadlineNamesEachStateOfThePrint()
    {
        var engine = new FakeEngineClient();
        var dialogs = new FakeDialogService
        {
            OnEnrolment = () =>
            {
                engine.AnchorOrigin = "enrolled";  // what the dialog's enrolment did
                engine.AnchorEnrolledAt = new DateTimeOffset(2026, 9, 4, 14, 2, 0, TimeSpan.Zero)
                    .ToUnixTimeSeconds();
                engine.RaiseNotification("anchor/enrolled",
                    Params(new { ok = true, detail = "", speechSeconds = 34.0 }));
            },
        };
        var shell = TestSession.Settings(engine, session: new FakeSession(), dialogs: dialogs);
        var voice = shell.Get<VoiceViewModel>();
        var status = shell.Line;

        await voice.RefreshAsync();
        Assert.False(voice.HasVoice);
        Assert.StartsWith("Tells you apart", voice.Headline);
        Assert.EndsWith("or set up now.", voice.Headline);
        Assert.Equal("Set up", voice.SetUpLabel);
        Assert.True(voice.SetUpVoiceCommand.CanExecute(null));

        await voice.SetUpVoiceCommand.ExecuteAsync(null);

        Assert.Equal(AnchorOrigin.Enrolled, voice.Origin);
        Assert.StartsWith("Set up on 4 Sep", voice.Headline);
        Assert.Contains("enrolment complete", status.LatestActivity);
        Assert.False(voice.Busy);

        engine.AnchorSessions = 1;
        await voice.RefreshAsync();
        Assert.EndsWith("refined automatically by 1 consultation since", voice.Headline);

        engine.AnchorOrigin = "accrued";
        engine.AnchorEnrolledAt = null;
        engine.AnchorSessions = 3;
        await voice.RefreshAsync();
        Assert.Equal("Learning automatically, 3 consultations so far", voice.Headline);

        engine.AnchorSessions = 12;
        await voice.RefreshAsync();
        Assert.Equal("Learned automatically from 12 consultations", voice.Headline);
        Assert.Equal("Redo", voice.SetUpLabel);
    }

    [Fact]
    public async Task ForgettingWaitsForTheConsultationIsConfirmedThenClearsAndReportsBack()
    {
        var engine = new FakeEngineClient { AnchorOrigin = "accrued", AnchorSessions = 4 };
        var dialogs = new FakeDialogService();
        var session = new FakeSession { ConsultationInProgress = true };
        var shell = TestSession.Settings(engine, session: session, dialogs: dialogs);
        var voice = shell.Get<VoiceViewModel>();
        var status = shell.Line;
        await voice.RefreshAsync();
        Assert.True(voice.ForgetVoiceCommand.CanExecute(null));

        // Nothing changes during a consultation
        await voice.ForgetVoiceCommand.ExecuteAsync(null);
        await voice.SetUpVoiceCommand.ExecuteAsync(null);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "anchor/clear");
        Assert.True(voice.HasVoice);
        Assert.Contains("finish the consultation", status.LatestActivity);

        session.ConsultationInProgress = false;
        dialogs.Answer = false;
        await voice.ForgetVoiceCommand.ExecuteAsync(null);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "anchor/clear");
        Assert.True(voice.HasVoice);

        dialogs.Answer = true;
        await voice.ForgetVoiceCommand.ExecuteAsync(null);
        Assert.Contains(engine.Requests, r => r.Method == "anchor/clear");
        Assert.False(voice.HasVoice);
        Assert.False(voice.ForgetVoiceCommand.CanExecute(null));
        Assert.Contains("forgotten", status.LatestActivity);
    }
}
