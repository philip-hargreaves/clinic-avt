using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Sessions;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;
using static ClinicAVT.App.Tests.Support.Waits;

namespace ClinicAVT.App.Tests.Features.Sessions;

public class SessionsViewModelTest
{
    private static (SessionsViewModel Sessions, ConsultationViewModel Consultation,
        FakeEngineClient Engine, StatusBarViewModel Status) Create(AppPreferences? preferences = null)
    {
        var (consultation, engine, _) = TestSession.Create(preferences);
        var sessions = new SessionsViewModel(
            new EngineApi(engine), consultation.Status, consultation, new FakeDialogService(), preferences);
        return (sessions, consultation, engine, consultation.Status);
    }

    private static void ScriptOneSession(FakeEngineClient engine)
    {
        engine.Responses["session/list"] = new
        {
            sessions = new[]
            {
                new
                {
                    id = "abc",
                    startedAt = "2026-08-17T10:15:00Z",
                    endedAt = "2026-08-17T10:23:41Z",
                    state = "finalised",
                    sampleRate = 16000,
                    label = "Elbow swelling",
                    editedAt = "2026-08-17T10:31:00Z",
                    audioSeconds = 542.0,  // a 16x replay, so the wall clock says 8:41 and the audio 9 min
                },
            },
        };
        engine.Responses["session/transcript"] = new
        {
            turns = new[]
            {
                new { firstFrame = 480000L, frameCount = 48000L, speaker = "doctor", text = "hello" },
            },
        };
        engine.Responses["session/note"] = new
        {
            text = "the note",
            style = "soap",
            detail = "concise",
            generatedAt = "2026-08-17T10:24:00Z",
            editedAt = "2026-08-17T10:31:00Z",
        };
        engine.Responses["session/patient"] = new
        {
            text = "the sheet",
            language = "en",
            editedAt = (string?)null,
            translation = new { language = "pl", text = "arkusz" },
        };
    }

    [Fact]
    public async Task RefreshListsSessionsWithLabelAndEditStampAndAMissingLabelFallsBackToTheDateAndTime()
    {
        var (vm, _, engine, _) = Create();
        ScriptOneSession(engine);

        await vm.RefreshAsync();

        var row = Assert.Single(vm.Sessions);
        Assert.Equal("abc", row.Id);
        Assert.Equal("Elbow swelling", row.Title);
        Assert.Equal("9 min", row.Duration);
        Assert.True(row.Edited);
        Assert.StartsWith("Edited ", row.EditedLabel);
        Assert.True(row.HasLabel);
        Assert.Equal("Elbow swelling", row.Heading);
        Assert.Equal($"{row.Started} · 9 min", row.Meta);
        Assert.True(row.MetaVisible);

        engine.Responses["session/list"] = new
        {
            sessions = new[]
            {
                new
                {
                    id = "abc",
                    startedAt = "2026-08-17T10:15:00Z",
                    endedAt = "2026-08-17T10:23:41Z",
                    state = "finalised",
                    sampleRate = 16000,
                    label = "",
                    editedAt = (string?)null,
                    demo = true,
                    hasReflection = true,
                },
            },
        };
        await vm.RefreshAsync();

        row = Assert.Single(vm.Sessions);
        Assert.Equal(row.Started, row.Title);
        Assert.False(row.Edited);
        Assert.True(row.Demo);
        Assert.True(row.HasReflection);
        Assert.False(row.HasLabel);
        Assert.Equal($"{row.Started} · {row.Duration}", row.Heading);  // said once
        Assert.False(row.MetaVisible);
    }

    [Fact]
    public async Task SelectingOpensTheSessionRenamingKeepsItOpenLeavingSavesEditsAndDeleteClosesFirst()
    {
        var (vm, consultation, engine, _) = Create();
        ScriptOneSession(engine);
        await vm.RefreshAsync();

        vm.Selected = vm.Sessions[0];
        await WaitUntilAsync(() => vm.DetailOpen);

        Assert.True(vm.DetailOpen);
        Assert.Contains(engine.Requests, c => c.Method == "session/open" && c.Params.Contains("abc"));
        Assert.Equal(SessionState.Review, consultation.State);
        Assert.Equal("the note", consultation.Note.ClinicalNoteText);
        Assert.Equal("the sheet", consultation.Note.PatientInfoText);
        Assert.Equal("arkusz", consultation.Note.TranslationText);
        Assert.Equal("pl", consultation.Note.TranslationLanguage);
        Assert.Equal("soap", consultation.Note.Style);
        Assert.True(consultation.Note.Edited);
        Assert.Single(consultation.Transcript.Turns);
        Assert.Equal("Elbow swelling", vm.DetailTitle);
        Assert.Contains("SOAP, concise", vm.DetailMeta);

        // A rewrite in another style is named under the title
        consultation.Note.Style = "prose";
        consultation.Note.Detail = "detailed";
        Assert.Contains("Prose, detailed", vm.DetailMeta);

        vm.DetailTitle = "Left elbow bursitis";
        await vm.RenameAsync();

        Assert.Contains(engine.Requests, c => c.Method == "session/label"
            && c.Params.Contains("Left elbow bursitis"));
        Assert.Equal("Left elbow bursitis", vm.Sessions[0].Title);
        Assert.Same(vm.Sessions[0], vm.Selected);
        Assert.True(vm.DetailOpen, "renaming must not close the open session");
        Assert.Equal(1, engine.Requests.Count(c => c.Method == "session/open"));

        consultation.Note.ClinicalNoteText = "the note, corrected";
        await vm.LeaveAsync();

        Assert.Contains(engine.Requests, c => c.Method == "note/update"
            && c.Params.Contains("the note, corrected"));
        Assert.Contains(engine.Requests, c => c.Method == "session/close");
        Assert.Equal(SessionState.Idle, consultation.State);
        Assert.False(vm.DetailOpen);

        // Delete closes the review it reopened before removing, then refreshes
        vm.Selected = vm.Sessions[0];
        await WaitUntilAsync(() => vm.DetailOpen);
        var lists = engine.Requests.Count(c => c.Method == "session/list");

        await vm.DeleteCommand.ExecuteAsync(vm.Selected);

        var close = engine.Requests.FindLastIndex(c => c.Method == "session/close");
        var delete = engine.Requests.FindIndex(c => c.Method == "session/delete");
        Assert.True(close >= 0 && delete > close, "close precedes delete");
        Assert.Equal(lists + 1, engine.Requests.Count(c => c.Method == "session/list"));
    }

    [Fact]
    public async Task AnOpenTheEngineRefusesLeavesTheDetailClosed()
    {
        var (vm, consultation, engine, _) = Create();
        ScriptOneSession(engine);
        engine.Failing.Add("session/open");
        await vm.RefreshAsync();

        vm.Selected = vm.Sessions[0];
        await WaitUntilAsync(() => engine.Requests.Any(r => r.Method == "session/open"));

        Assert.False(vm.DetailOpen);
        Assert.Equal(SessionState.Idle, consultation.State);
    }

    [Fact]
    public async Task EngineErrorsLandInTheStatusLogAndAnEmptyStoreIsAnEmptyListNotAnError()
    {
        var (vm, _, engine, status) = Create();
        engine.Failing.Add("session/list");

        await vm.RefreshAsync();

        Assert.Empty(vm.Sessions);
        Assert.Contains("could not list consultations", status.LatestActivity);

        engine.Failing.Remove("session/list");
        engine.Responses["session/list"] = new { sessions = Array.Empty<object>() };
        await vm.RefreshAsync();

        Assert.Empty(vm.Sessions);
        Assert.False(vm.EmptyBecauseOff, "no preference known: a plain empty list");
    }

    [Fact]
    public async Task AnEmptyListExplainsItselfWhenRetentionIsOff()
    {
        var (vm, _, engine, _) = Create(new AppPreferences(
            Path.Combine(Path.GetTempPath(), Path.GetRandomFileName())));
        engine.Responses["session/list"] = new { sessions = Array.Empty<object>() };

        await vm.RefreshAsync();
        Assert.True(vm.EmptyBecauseOff, "keep is off by default and nothing is stored");

        ScriptOneSession(engine);  // history recorded while keep was on still shows
        await vm.RefreshAsync();
        Assert.False(vm.EmptyBecauseOff);
        Assert.Single(vm.Sessions);
    }

    [Fact]
    public async Task EnteringOpensTheMostRecentConsultationAndGoingToRecordEndsAStoredReviewOnly()
    {
        var (vm, consultation, engine, _) = Create();
        ScriptOneSession(engine);

        await vm.CloseStoredReviewAsync();
        Assert.DoesNotContain(engine.Requests, c => c.Method == "session/close");

        await vm.EnterAsync();
        await WaitUntilAsync(() => vm.DetailOpen);
        Assert.Same(vm.Sessions[0], vm.Selected);
        Assert.Contains(engine.Requests, c => c.Method == "session/open" && c.Params.Contains("abc"));
        Assert.True(consultation.ReviewingStored);

        await vm.CloseStoredReviewAsync();

        Assert.Contains(engine.Requests, c => c.Method == "session/close");
        Assert.Null(vm.Selected);
        Assert.False(vm.DetailOpen);
        Assert.False(consultation.ReviewingStored);
        Assert.Equal(SessionState.Idle, consultation.State);
    }

    [Fact]
    public async Task EnteringDuringTheLiveReviewShowsItWithoutReopeningAndGoingBackKeepsIt()
    {
        var (vm, consultation, engine, _) = Create();
        await consultation.StartRecordingAsync();
        await consultation.StopRecordingAsync();
        engine.RaiseNotification("note/ready");
        engine.RaiseNotification("patient/ready");
        Assert.Equal(SessionState.Review, consultation.State);
        engine.Responses["session/list"] = new
        {
            sessions = new[]
            {
                new { id = "s1", startedAt = "2026-09-25T14:09:00Z", endedAt = "2026-09-25T14:18:00Z", state = "finalised", sampleRate = 16000, label = "Left elbow swelling", editedAt = "", audioSeconds = 540.0 },
                new { id = "abc", startedAt = "2026-08-17T10:15:00Z", endedAt = "2026-08-17T10:23:41Z", state = "finalised", sampleRate = 16000, label = "Elbow swelling", editedAt = "", audioSeconds = 542.0 },
            },
        };

        await vm.EnterAsync();

        Assert.Equal("s1", vm.Selected?.Id);
        Assert.True(vm.DetailOpen);
        Assert.DoesNotContain(engine.Requests, c => c.Method == "session/open");
        Assert.False(consultation.ReviewingStored);

        await vm.CloseStoredReviewAsync();
        Assert.DoesNotContain(engine.Requests, c => c.Method == "session/close");
        Assert.Equal(SessionState.Review, consultation.State);
    }

    [Fact]
    public async Task RowsGroupByDayAndTheQueryNarrowsThem()
    {
        var (vm, _, engine, _) = Create();
        engine.Responses["session/list"] = new
        {
            sessions = new[]
            {
                new { id = "b", startedAt = "2026-08-18T09:00:00Z", endedAt = "2026-08-18T09:09:00Z", state = "finalised", sampleRate = 16000, label = "Knee pain", editedAt = "", audioSeconds = 540.0 },
                new { id = "a", startedAt = "2026-08-17T10:15:00Z", endedAt = "2026-08-17T10:23:41Z", state = "finalised", sampleRate = 16000, label = "Elbow swelling", editedAt = "", audioSeconds = 542.0 },
            },
        };

        await vm.RefreshAsync();

        Assert.Equal(2, vm.Groups.Count);
        Assert.Equal("Knee pain", vm.Groups[0][0].Title);
        Assert.Equal("Elbow swelling", vm.Groups[1][0].Title);

        vm.Query = "elbow";

        Assert.Single(vm.Groups);
        Assert.Equal("Elbow swelling", vm.Groups[0][0].Title);
    }
}
