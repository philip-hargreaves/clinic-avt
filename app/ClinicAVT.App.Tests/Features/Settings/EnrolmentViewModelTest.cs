using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Features.Settings;

public class EnrolmentViewModelTest
{
    [Fact]
    public async Task StartAsksTheEngineProgressCountsClearSpeechThenFinishAndSuccessClose()
    {
        var engine = new FakeEngineClient();
        using var enrolment = new EnrolmentViewModel(new EngineApi(engine), micId: "mic-7", seconds: 30);
        Assert.Equal(EnrolmentState.Ready, enrolment.State);
        Assert.Equal("Start", enrolment.PrimaryText);
        Assert.True(enrolment.KeepsOpen);
        Assert.Equal("Cancel", enrolment.CloseText);

        // Progress before Start belongs to someone else's window
        engine.RaiseNotification("anchor/progress",
            Params(new { elapsed = 3.0, speech = 1.0, level = 0.9, clipped = true }));
        Assert.Equal(0, enrolment.Level);
        Assert.Equal(EnrolmentState.Ready, enrolment.State);

        await enrolment.PrimaryCommand.ExecuteAsync(null);

        var request = Assert.Single(engine.Requests, r => r.Method == "anchor/enrol");
        Assert.Contains("\"seconds\":30", request.Params);
        Assert.Contains("\"id\":\"mic-7\"", request.Params);
        Assert.Equal(EnrolmentState.Recording, enrolment.State);
        Assert.True(enrolment.Recording);
        Assert.Equal("Finish", enrolment.PrimaryText);

        engine.RaiseNotification("anchor/progress",
            Params(new { elapsed = 12.0, speech = 10.0, level = 0.7, clipped = false }));
        Assert.Equal(0.7, enrolment.Level);
        Assert.Equal(0.4, enrolment.Progress, 10);
        Assert.False(enrolment.EnoughCaptured);
        Assert.StartsWith("Listening. Read to the end", enrolment.StatusLine);

        engine.RaiseNotification("anchor/progress",
            Params(new { elapsed = 25.0, speech = 22.0, level = 0.5, clipped = false }));
        Assert.False(enrolment.EnoughCaptured, "past the engine's 20 s, short of the bar's 25");

        engine.RaiseNotification("anchor/progress",
            Params(new { elapsed = 30.0, speech = 26.0, level = 0.4, clipped = false }));
        Assert.Equal(1.0, enrolment.Progress);
        Assert.True(enrolment.EnoughCaptured);
        Assert.StartsWith("Enough captured", enrolment.StatusLine);

        await enrolment.PrimaryCommand.ExecuteAsync(null);
        Assert.Contains(engine.Requests, r => r.Method == "anchor/enrol/finish");

        engine.RaiseNotification("anchor/enrolled",
            Params(new { ok = true, detail = "", speechSeconds = 34.0 }));
        Assert.Equal(EnrolmentState.Succeeded, enrolment.State);
        Assert.Equal("Done", enrolment.PrimaryText);
        Assert.False(enrolment.KeepsOpen);
        Assert.Equal("", enrolment.CloseText);
        Assert.Equal(0, enrolment.Level);
        Assert.True(await enrolment.Outcome);
    }

    [Fact]
    public async Task ADownEngineOrARefusalSaysWhyAndOffersAnotherGoAndDismissalKeepsNothing()
    {
        var engine = new FakeEngineClient();
        engine.SetConnected(false);
        var enrolment = new EnrolmentViewModel(new EngineApi(engine));

        await enrolment.PrimaryCommand.ExecuteAsync(null);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "anchor/enrol");
        Assert.Equal(EnrolmentState.Failed, enrolment.State);
        Assert.Equal("That did not work: recording is not available yet.", enrolment.StatusLine);

        engine.SetConnected(true);
        await enrolment.PrimaryCommand.ExecuteAsync(null);
        Assert.Equal(EnrolmentState.Recording, enrolment.State);

        engine.RaiseNotification("anchor/enrolled", Params(new
        {
            ok = false,
            detail = "not enough clear speech: 12 s of 20 s needed",
            speechSeconds = 12.0,
        }));

        Assert.Equal(EnrolmentState.Failed, enrolment.State);
        Assert.Contains("12 s of 20 s", enrolment.StatusLine);
        Assert.Equal("Try again", enrolment.PrimaryText);

        await enrolment.PrimaryCommand.ExecuteAsync(null);
        Assert.Equal(EnrolmentState.Recording, enrolment.State);
        Assert.Equal(2, engine.Requests.Count(r => r.Method == "anchor/enrol"));

        enrolment.Dismiss();
        Assert.Contains(engine.Requests, r => r.Method == "anchor/enrol/cancel");
        Assert.False(await enrolment.Outcome);
        enrolment.Dispose();
    }
}
