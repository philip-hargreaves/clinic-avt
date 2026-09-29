using System.Text.Json;
using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using static ClinicAVT.App.Tests.Features.Settings.GuidanceDocumentsViewModelTest;
using static ClinicAVT.App.Tests.Support.Waits;

namespace ClinicAVT.App.Tests.Features.Settings;

public class PrivacySettingsTest
{
    private static AppPreferences Preferences() => new(new MemoryPreferencesStore());

    [Fact]
    public void SeedDataFollowsTheSwitchPersistsAndALaunchWithItOnSeedsQuietly()
    {
        var preferences = Preferences();
        var engine = new FakeEngineClient();
        var shell = TestSession.Settings(engine, preferences);
        var privacy = shell.Get<PrivacySettings>();
        var status = shell.Line;
        Assert.False(privacy.SeedDataEnabled);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "demo/seed");

        privacy.SeedDataEnabled = true;
        Assert.True(preferences.SeedDataEnabled);
        Assert.True(engine.SamplesSeeded);
        Assert.Contains("8 sample consultations added", status.LatestActivity);

        privacy.SeedDataEnabled = false;
        Assert.False(preferences.SeedDataEnabled);
        Assert.False(engine.SamplesSeeded);
        Assert.Contains("8 sample consultations removed", status.LatestActivity);

        // A relaunch with the switch on and the samples present seeds once and says nothing
        preferences.SeedDataEnabled = true;
        engine.SamplesSeeded = true;
        engine.Requests.Clear();
        var relaunched = TestSession.Settings(engine, preferences);
        var again = relaunched.Get<PrivacySettings>();

        Assert.True(again.SeedDataEnabled);
        Assert.Single(engine.Requests, r => r.Method == "demo/seed");
        Assert.DoesNotContain("sample", relaunched.Line.LatestActivity);
    }

    [Fact]
    public async Task DeleteAllAsksFirstThenErasesConsultationsOnlyAndTurnsSeedDataOff()
    {
        var preferences = Preferences();
        var engine = new FakeEngineClient { StoredSessions = 3 };
        engine.GuidanceDocuments.Add(Document(1, "Gout", "ready", 41));
        var dialogs = new FakeDialogService { Answer = false };
        var shell = TestSession.Settings(engine, preferences, dialogs: dialogs);
        var settings = shell.Get<SettingsViewModel>();
        settings.Privacy.SeedDataEnabled = true;

        await settings.Privacy.DeleteAllConsultationsCommand.ExecuteAsync(null);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/deleteAll");
        Assert.True(settings.Privacy.SeedDataEnabled);

        dialogs.Answer = true;
        await settings.Privacy.DeleteAllConsultationsCommand.ExecuteAsync(null);
        Assert.Single(engine.Requests, r => r.Method == "session/deleteAll");
        Assert.Contains("11 consultations deleted", shell.Line.LatestActivity);
        Assert.False(settings.Privacy.SeedDataEnabled);
        Assert.False(preferences.SeedDataEnabled);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "demo/clear");  // nothing left to clear
        Assert.Equal(0, engine.StoredSessions);
        Assert.Single(settings.Documents.Documents);  // guideline documents are not consultations
    }

    // A date cannot say what a backup holds, so the engine counts what is in no backup
    [Fact]
    public async Task DeleteAllSaysWhatNoBackupHoldsAndKeepsReflectionsUnlessTicked()
    {
        var preferences = Preferences();
        var engine = new FakeEngineClient { StoredSessions = 40 };
        var dialogs = new FakeDialogService();
        var privacy = TestSession.Settings(engine, preferences, dialogs: dialogs).Get<PrivacySettings>();

        await privacy.DeleteAllConsultationsCommand.ExecuteAsync(null);
        Assert.Equal("No consultations are backed up. This can't be undone.", dialogs.LastContent);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "archive/summary");
        Assert.Contains("\"deleteReflections\":false", engine.Requests.Single(r => r.Method == "session/deleteAll").Params);

        preferences.LastBackup = new LastBackup(
            "2026-07-01T00:00:00Z", "2026-09-01T00:00:00Z", "2026-09-01T08:30:00Z", 32);
        engine.Responses["archive/summary"] = new { consultations = 38, reflections = 12, unfinished = 0, uncovered = 6 };
        dialogs.Ticked = true;
        await privacy.DeleteAllConsultationsCommand.ExecuteAsync(null);

        Assert.Equal("6 consultations aren't backed up. This can't be undone.", dialogs.LastContent);
        var covered = JsonDocument.Parse(engine.Requests.Single(r => r.Method == "archive/summary").Params)
            .RootElement.GetProperty("covered");
        Assert.Equal("2026-09-01T08:30:00Z", covered.GetProperty("at").GetString());
        Assert.Contains("\"deleteReflections\":true", engine.Requests.Last(r => r.Method == "session/deleteAll").Params);
        Assert.Contains("Last backup: 1 Sep 2026, 32 consultations.", privacy.BackupDescription);
    }

    [Fact]
    public async Task KeepConsultationsDefaultsOffAndTurningOnIsConfirmedNeverJustToggled()
    {
        var preferences = Preferences();
        var asked = 0;
        var dialogs = new FakeDialogService { Answer = false, OnConfirm = () => asked++ };
        var privacy = TestSession.Settings(preferences: preferences, dialogs: dialogs).Get<PrivacySettings>();
        Assert.False(privacy.KeepConsultations, "save nothing unless the clinician opts in");

        privacy.KeepConsultations = true;
        await WaitUntilAsync(() => asked == 1);
        Assert.Equal(1, asked);
        Assert.False(privacy.KeepConsultations, "declined: the toggle stays off");
        Assert.False(preferences.KeepConsultations, "and nothing was persisted");

        dialogs.Answer = true;
        privacy.KeepConsultations = true;
        await WaitUntilAsync(() => asked == 2);
        Assert.Equal(2, asked);
        Assert.True(privacy.KeepConsultations);
        Assert.True(preferences.KeepConsultations);

        privacy.KeepConsultations = false;  // turning off needs no confirmation
        Assert.Equal(2, asked);
        Assert.False(preferences.KeepConsultations);
    }
}
