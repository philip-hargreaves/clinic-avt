using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Appraisal;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Sessions;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;

namespace ClinicAVT.App.Tests.Shell;

/// <summary>The pure helpers the views lean on.</summary>
public class ShellHelpersTest
{
    // Engine, session and RPC terms stay off the status line. The detail goes to the log
    [Fact]
    public void AFailureTheEngineDidNotExplainReadsAsPlainWordsAndIsLogged()
    {
        var log = new ListLogger();
        Assert.Equal("ClinicAVT didn't respond in time", EngineWords.Reason(new TaskCanceledException(), log));
        Assert.Equal("something went wrong",
            EngineWords.Reason(new IOException("pipe transport is closed"), log));
        Assert.Contains(log.Lines, line => line.Contains("pipe transport is closed"));
        Assert.Equal(2, log.Lines.Count);

        Assert.Equal("the speech recognition model is not installed", ModelNames.Missing(["asr"]));
        Assert.Equal("the speech recognition, speech detection and speaker recognition models are not installed",
            ModelNames.Missing(["asr", "vad", "diarisation", "segmentation"]));
    }

    // An exception out of an async void handler would end the app
    [Fact]
    public async Task AFailingUiHandlerIsLoggedAndGoesNoFurther()
    {
        var log = new ListLogger();
        await UiEvent.RunAsync(() => throw new InvalidOperationException("store gone"), log,
            "SessionsView.OnTitleCommitted");
        await UiEvent.RunAsync(() => Task.CompletedTask, log, "SessionsView.OnTitleCommitted");

        Assert.Equal(["Error: SessionsView.OnTitleCommitted failed"], log.Lines);
    }

    [Fact]
    public void TheLevelCurveRestsOnRoomNoiseAndSpreadsSpeechOverTheSwing()
    {
        Assert.Equal(1.0, LevelCurve.RingScale(-1));
        Assert.Equal(1.0, LevelCurve.RingScale(0.2), 10);  // room noise: at rest
        Assert.InRange(LevelCurve.RingScale(0.55), 1.18, 1.25);  // ordinary speech moves the ring visibly
        Assert.Equal(1.0, LevelCurve.GlowScale(0));
        Assert.Equal(LevelCurve.GlowScale(1), LevelCurve.GlowScale(4), 10);  // loud input saturates
        Assert.True(LevelCurve.GlowAlpha(0.5) < LevelCurve.GlowAlpha(0.6));
        Assert.True(LevelCurve.RingAlpha(0) < LevelCurve.RingAlpha(1));
    }

    [Theory]
    [InlineData("https://www.nice.org.uk/guidance/ng100", true)]
    [InlineData("http://example.test", true)]
    [InlineData("Gout.md", false)]
    [InlineData("javascript:alert(1)", false)]
    [InlineData("", false)]
    public void OnlyWebAddressesOpenInTheBrowser(string link, bool expected) =>
        Assert.Equal(expected, WebLinks.IsWeb(link));

    [Fact]
    public async Task GoingToRecordEndsAStoredReviewAndLeavingAppraisalClosesTheOpenReflection()
    {
        var navigation = new RecordingNavigationService();
        var (shell, consultation, sessions, appraisals) = Shell(navigation);
        var engine = consultation.Engine;

        engine.StoredNote = "the stored note";
        await consultation.Session.OpenStoredSessionAsync("abc");
        Assert.True(consultation.Session.ReviewingStored);

        await shell.NavigateCommand.ExecuteAsync(Routes.Sessions);
        Assert.True(consultation.Session.ReviewingStored);
        Assert.Equal(Routes.Sessions, navigation.Current);

        await shell.NavigateCommand.ExecuteAsync(Routes.Consultation);
        Assert.False(consultation.Session.ReviewingStored);
        Assert.Equal(Routes.Consultation, navigation.Current);

        engine.Reflections.Add(("r1", "2026-09-01T10:00:00Z", "", "listen longer", ""));
        await shell.NavigateCommand.ExecuteAsync(Routes.Appraisals);
        await appraisals.RefreshAsync();
        var card = Assert.Single(appraisals.Cards);
        await appraisals.ToggleAsync(card);
        Assert.True(card.Expanded);

        await shell.NavigateCommand.ExecuteAsync(Routes.Help);
        Assert.False(card.Expanded);
        Assert.Equal(Routes.Help, navigation.Current);
        Assert.Null(sessions.Selected);

        await shell.ShowSettingsCommand.ExecuteAsync(null);
        Assert.Equal(Routes.Settings, navigation.Current);
    }

    private static (ShellViewModel Shell,
        (ConsultationViewModel Session, FakeEngineClient Engine) Consultation,
        SessionsViewModel Sessions, AppraisalsViewModel Appraisals) Shell(RecordingNavigationService navigation)
    {
        var (session, engine, _) = TestSession.Create();
        var api = new EngineApi(engine);
        var sessions = new SessionsViewModel(api, session.Status, session, new FakeDialogService());
        var appraisals = new AppraisalsViewModel(
            api, new InlineDispatcher(), session.Status, new FakeClipboard(), new FakeFilePicker(),
            new FakeDialogService());
        return (new ShellViewModel(navigation, sessions, appraisals), (session, engine), sessions, appraisals);
    }
}
