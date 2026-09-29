using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using static ClinicAVT.App.Tests.Features.Settings.NoteModelSettingsTest;

namespace ClinicAVT.App.Tests.Features.Settings;

public class SettingsViewModelTest
{
    // Everything that restarts or reconfigures the engine waits for the consultation to end. A
    // finished one on screen blocks nothing, and deleting everything closes its review first so
    // the screen never shows erased data
    [Fact]
    public async Task EngineChangesWaitForTheConsultationToEndAndDeleteAllClosesTheReviewOnScreen()
    {
        var preferences = new AppPreferences(new MemoryPreferencesStore());
        var engine = TieredEngine();
        engine.StoredSessions = 3;
        var session = new FakeSession { ConsultationInProgress = true };
        var dialogs = new FakeDialogService();
        var shell = TestSession.Settings(engine, preferences, session, dialogs);
        var settings = shell.Get<SettingsViewModel>();
        var status = shell.Line;

        await settings.Privacy.DeleteAllConsultationsCommand.ExecuteAsync(null);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/deleteAll");
        Assert.Contains("finish the consultation", status.LatestActivity);

        settings.Appearance.NpuTranscription = true;
        Assert.False(settings.Appearance.NpuTranscription);
        Assert.False(preferences.NpuTranscription);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "asr/device");

        settings.NoteModel.NoteModelIndex = 2;
        Assert.Equal(1, settings.NoteModel.NoteModelIndex);
        Assert.Equal("auto", preferences.NoteTier);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "note/tier");

        session.ConsultationInProgress = false;
        session.ReviewedSessionId = "s1";
        await settings.Privacy.BackUpCommand.ExecuteAsync(null);
        Assert.Equal(1, dialogs.BackupsRun);

        settings.Appearance.NpuTranscription = true;
        Assert.Contains(engine.Requests, r => r.Method == "asr/device");

        settings.NoteModel.NoteModelIndex = 2;
        Assert.Contains(engine.Requests, r => r.Method == "note/tier");

        await settings.Privacy.DeleteAllConsultationsCommand.ExecuteAsync(null);
        Assert.Equal(1, session.ReviewsEnded);
        Assert.Contains(engine.Requests, r => r.Method == "session/deleteAll");
        Assert.DoesNotContain("finish the consultation", status.LatestActivity);
    }
}
