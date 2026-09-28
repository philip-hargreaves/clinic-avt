using System.Globalization;
using System.Text.Json;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Demo;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Features.Consultation;

public class ImportRecordingTest
{
    private static readonly JsonElement InspectSent =
        Fixtures.Load("recording-inspect.json").GetProperty("request").GetProperty("params");

    private static readonly JsonElement ImportSent =
        Fixtures.Load("session-import.json").GetProperty("request").GetProperty("params");

    private static string FixturePath => ImportSent.GetProperty("path").GetString()!;

    private static string FixtureStartedAt => ImportSent.GetProperty("startedAt").GetString()!;

    private static ImportRecordingViewModel Dialog(FakeEngineClient engine, FakeFilePicker picker) =>
        new(new EngineApi(engine), picker, FakeTimeProvider.London());

    [Fact]
    public async Task AChosenRecordingIsReadThenImportedAtTheChosenTimeInUtc()
    {
        var engine = new FakeEngineClient();
        var picker = new FakeFilePicker { OpenPath = FixturePath };
        var import = Dialog(engine, picker);
        Assert.True(import.NoFile);
        Assert.False(import.CanImport);

        await import.ChooseCommand.ExecuteAsync(null);

        Assert.True(JsonElement.DeepEquals(InspectSent, engine.Sent("recording/inspect")));
        Assert.Equal("Home visit 26 Sep.m4a", import.FileName);
        Assert.Equal("12:40", import.LengthText);
        // 13:05 UTC is 14:05 in London in September
        Assert.Equal(new DateTime(2026, 9, 26), import.Day!.Value.Date);
        Assert.Equal(new TimeSpan(14, 5, 0), import.Time);
        Assert.True(import.CanImport);
        Assert.Equal("", import.Problem);
        Assert.Equal((FixturePath, FixtureStartedAt, 760.4),
            (import.Result!.Path, import.Result.StartedAt, import.Result.Seconds));

        import.Day = new DateTimeOffset(2026, 9, 25, 0, 0, 0, TimeSpan.FromHours(1));
        import.Time = new TimeSpan(9, 30, 0);
        Assert.Equal("2026-09-25T08:30:00Z", import.Result!.StartedAt);

        // The clock reads 10:00 in London: 11:00 today has not happened yet
        Assert.Equal(new DateTime(2026, 9, 27), import.Today.Date);
        import.Day = import.Today;
        import.Time = new TimeSpan(11, 0, 0);
        Assert.Contains("future", import.Problem);
        Assert.Null(import.Result);
        import.Time = new TimeSpan(9, 45, 0);
        Assert.Equal("2026-09-27T08:45:00Z", import.Result!.StartedAt);

        // The shell refuses on the inspected length; the engine has no length rule
        engine.RecordingSeconds = 29.4;
        await import.UseFileAsync(@"C:\Users\clinician\Downloads\Voice memo.wav");
        Assert.True(import.TooShort);
        Assert.Contains("too short", import.Problem);
        Assert.Null(import.Result);

        var inspections = engine.Requests.Count(r => r.Method == "recording/inspect");
        await import.UseFileAsync(@"C:\Users\clinician\Downloads\Referral.pdf");
        Assert.Equal(inspections, engine.Requests.Count(r => r.Method == "recording/inspect"));
        Assert.Contains("not a recording", import.Problem);
        Assert.False(import.CanImport);
    }

    // A bundled example goes through the same inspection and import as a chosen file
    [Fact]
    public async Task AnExampleImportsLikeAChosenFileAndAChosenFileReplacesIt()
    {
        var engine = new FakeEngineClient();
        var picker = new FakeFilePicker { OpenPath = FixturePath };
        var import = new ImportRecordingViewModel(new EngineApi(engine), picker, FakeTimeProvider.London(),
            [new DemoTrack("Elbow swelling", @"C:\demo\elbow.wav"), new DemoTrack("Chest pain", @"C:\demo\chest.wav")]);
        Assert.Equal(["Elbow swelling", "Chest pain"], import.ExampleNames);
        Assert.True(import.ExamplesVisible);
        Assert.Equal(-1, import.ExampleIndex);

        import.ExampleIndex = 1;
        await Waits.WaitUntilAsync(() => import.Inspected);

        Assert.Equal(@"C:\demo\chest.wav", import.Path);
        Assert.False(import.ShowFile);  // the example list already names it
        Assert.True(import.ShowDropArea);
        Assert.False(import.ShowWhen);  // dated when added
        Assert.True(import.CanImport);
        Assert.Equal(@"C:\demo\chest.wav", import.Result!.Path);

        await import.ChooseCommand.ExecuteAsync(null);
        Assert.Equal(-1, import.ExampleIndex);
        Assert.Equal("Home visit 26 Sep.m4a", import.FileName);
        Assert.True(import.ShowFile);
        Assert.True(import.ShowWhen);

        var none = new ImportRecordingViewModel(new EngineApi(engine), picker, FakeTimeProvider.London(), []);
        Assert.False(none.ExamplesVisible);
    }

    [Fact]
    public async Task AnImportEndsTheOpenReviewThenWalksTheFinaliseStagesIntoReview()
    {
        var dialogs = new FakeDialogService();
        var (session, engine, _) = TestSession.Create(dialogs: dialogs);
        var controls = new SessionControlsViewModel(session, TestSession.Mic());
        var header = new ConsultationHeaderViewModel(session);
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready", Params(new { text = "note" }));
        Assert.Equal(SessionState.Review, session.State);

        // Cancelled, nothing changes
        await controls.ImportRecordingCommand.ExecuteAsync(null);
        Assert.Equal(SessionState.Review, session.State);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/import");

        var phases = new List<FinalisePhase>();
        session.PropertyChanged += (_, e) =>
        {
            // Closing the review resets the phase first
            if (e.PropertyName == nameof(ConsultationViewModel.Phase) && session.Phase != FinalisePhase.None)
            {
                phases.Add(session.Phase);
            }
        };
        dialogs.Import = new RecordingImport(FixturePath, FixtureStartedAt, 760.4);
        Assert.True(controls.ImportRecordingCommand.CanExecute(FixturePath));
        await controls.ImportRecordingCommand.ExecuteAsync(FixturePath);

        Assert.Equal([null, FixturePath], dialogs.ImportsShown);
        var methods = engine.Requests.Select(r => r.Method).ToList();
        Assert.True(methods.LastIndexOf("session/close") < methods.IndexOf("session/import"),
            "the review closes first");
        // Keep consultations goes with it, as with a recording
        Assert.True(JsonElement.DeepEquals(ImportSent, engine.Sent("session/import")));
        Assert.Equal(
            [FinalisePhase.Sealing, FinalisePhase.Transcript, FinalisePhase.Speakers, FinalisePhase.Turns,
                FinalisePhase.Note],
            phases);
        Assert.Equal(SessionState.Finalising, session.State);
        Assert.True(controls.FinalisingVisible);
        Assert.Equal("12:40", controls.ElapsedLabel);
        // Headed with the import's own time
        Assert.Equal(SessionText.Heading(DateTimeOffset.Parse(FixtureStartedAt, CultureInfo.InvariantCulture)), header.Title);

        engine.RaiseNotification("note/partial", Params(new { text = "The" }));
        Assert.True(controls.PanesVisible);
        engine.RaiseNotification("note/ready", Params(new { text = "The note" }));
        Assert.Equal(SessionState.Review, session.State);
        Assert.Equal("s1", session.LiveReviewId);
    }

    [Fact]
    public async Task AnImportShowsOneFigureAcrossItsStagesUntilTheEngineSealsIt()
    {
        var dialogs = new FakeDialogService { Import = new RecordingImport(FixturePath, FixtureStartedAt, 760.4) };
        var (session, engine, _) = TestSession.Create(dialogs: dialogs);
        var controls = new SessionControlsViewModel(session, TestSession.Mic());
        engine.HoldImport = true;
        engine.ImportStages.Clear();

        var importing = controls.ImportRecordingCommand.ExecuteAsync(null);
        Assert.Equal("Preparing", controls.FinalisingLabel);
        Assert.True(controls.ImportCancelVisible);
        Assert.True(controls.CancelImportCommand.CanExecute(null));

        // Reading the file and finding the speech both prepare
        engine.ImportProgress("reading", 3);
        Assert.Equal("Preparing · 3%", controls.FinalisingLabel);
        engine.RaiseNotification("session/progress", Params(new { stage = "transcript" }));
        engine.ImportProgress("speech", 12);
        Assert.Equal("Preparing · 12%", controls.FinalisingLabel);
        Assert.Equal("Preparing · 12%", session.Status.LatestActivity);
        engine.ImportProgress("transcribing", 60);
        Assert.Equal("Transcribing · 60%", controls.FinalisingLabel);
        Assert.Equal("Transcribing · 60%", session.Status.LatestActivity);

        // The figure's last five points are the finalise, which names no percentage
        engine.ImportProgress("finalising", 95);
        Assert.Equal("Finalising", controls.FinalisingLabel);
        Assert.Equal("Finalising", session.Status.LatestActivity);
        engine.RaiseNotification("session/progress", Params(new { stage = "speakers" }));
        Assert.Equal("Finalising", controls.FinalisingLabel);
        engine.ImportProgress("finalising", 100);
        Assert.Equal("Finalising", session.Status.LatestActivity);

        engine.FinishImport();
        await importing;
        Assert.False(session.Importing);
        Assert.False(controls.ImportCancelVisible);
        Assert.Equal(SessionState.Finalising, session.State);
        engine.RaiseNotification("note/ready", Params(new { text = "The note" }));
        Assert.Equal(SessionState.Review, session.State);
    }

    [Fact]
    public async Task CancellingAnImportReturnsToReadyToStartWithoutAnError()
    {
        var dialogs = new FakeDialogService { Import = new RecordingImport(FixturePath, FixtureStartedAt, 760.4) };
        var (session, engine, _) = TestSession.Create(dialogs: dialogs);
        var controls = new SessionControlsViewModel(session, TestSession.Mic());
        engine.HoldImport = true;

        var importing = controls.ImportRecordingCommand.ExecuteAsync(null);
        engine.ImportProgress("transcribing", 55);
        await controls.CancelImportCommand.ExecuteAsync(null);
        await importing;

        Assert.Contains(engine.Requests, r => r.Method == "session/cancel");
        Assert.Equal(SessionState.Idle, session.State);
        Assert.True(controls.IdleVisible);
        Assert.Equal("Ready to start", controls.StartLabel);
        Assert.False(controls.ImportCancelVisible);
        Assert.Equal("Cancelled", session.Status.LatestActivity);
        Assert.Equal("", session.Note.ClinicalNoteText);
        Assert.True(controls.ImportRecordingCommand.CanExecute(null), "another import can start");
    }

    [Fact]
    public async Task NoImportDuringARecordingAndARefusalLeavesItIdle()
    {
        var import = new RecordingImport(FixturePath, FixtureStartedAt, 760.4);
        var dialogs = new FakeDialogService { Import = import };
        var (session, engine, _) = TestSession.Create(dialogs: dialogs);
        var controls = new SessionControlsViewModel(session, TestSession.Mic());

        await session.StartRecordingAsync();
        Assert.False(controls.ImportRecordingCommand.CanExecute(null));
        await session.Recorder.ImportRecordingAsync(import);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/import");
        Assert.Equal(SessionState.Recording, session.State);

        await session.CancelRecordingAsync();
        engine.FailNext = method => method == "session/import"
            ? new EngineErrorException(-32000, "recording too short", null)
            : null;
        await controls.ImportRecordingCommand.ExecuteAsync(null);
        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal("Could not import the recording: recording too short", session.Status.LatestActivity);
    }
}
