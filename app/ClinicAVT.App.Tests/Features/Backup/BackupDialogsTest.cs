using System.Text.Json;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Backup;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Features.Backup;

public class BackupDialogsTest
{
    private static readonly string[] BackedUpIds = ["a1", "b2"];

    private static AppPreferences TempPreferences() =>
        new(Path.Combine(Path.GetTempPath(), Path.GetRandomFileName()));

    [Fact]
    public async Task AFirstBackupOffersAPasswordThenRemovesExactlyWhatItHolds()
    {
        var engine = new FakeEngineClient();
        engine.Responses["archive/summary"] = new { consultations = 38, reflections = 12, unfinished = 1, uncovered = 0 };
        engine.Responses["session/remove"] = new { removed = 2 };
        var picker = new FakeFilePicker { SavePath = @"C:\Users\gp\OneDrive - NHS\Documents\b.clinicavt" };
        var launcher = new FakeLauncher();
        var preferences = TempPreferences();
        using var backup = new BackupViewModel(new EngineApi(engine), picker, launcher, preferences,
            new InlineDispatcher(), FakeTimeProvider.London(),
            name => name is "OneDriveCommercial" or "OneDrive" ? @"C:\Users\gp\OneDrive - NHS" : null);
        await backup.LoadAsync();

        Assert.Equal("38 consultations on this computer.", backup.CountLine);
        Assert.False(backup.CanStart);  // no password yet
        const string chosen = "harbour lights";
        backup.Password = chosen;
        backup.PasswordAgain = chosen;
        Assert.True(backup.CanStart);
        await backup.PrimaryCommand.ExecuteAsync(null);

        Assert.Equal(["ClinicAVT backup 27 Sep 2026"], picker.SuggestedNames);
        var sent = engine.Sent("archive/backup");
        Assert.Equal(("", "", chosen), (sent.GetProperty("from").GetString(), sent.GetProperty("to").GetString(),
            sent.GetProperty("password").GetString()));
        Assert.True(backup.BlocksClose);
        Assert.Contains("work OneDrive", backup.OneDriveLine);

        engine.RaiseNotification("archive/progress", Params(new { job = "backup", phase = "writing", done = 12, total = 38 }));
        Assert.Equal("Backing up 12 of 38 consultations…", backup.ProgressText);
        engine.RaiseNotification("archive/done", Params(new
        {
            job = "backup",
            dryRun = false,
            consultations = 38,
            reflections = 12,
            skipped = 0,
            from = "",
            to = "",
            createdAt = "2026-09-27T09:05:00Z",
            ids = BackedUpIds,
        }));
        Assert.Equal("38 consultations backed up and checked.", backup.DoneLine);
        Assert.Equal("Saved as b in Documents.", backup.SavedLine);
        Assert.Equal(new LastBackup("", "", "2026-09-27T09:05:00Z", 38), preferences.LastBackup);

        backup.AskRemoveCommand.Execute(null);
        Assert.Equal("Remove", backup.PrimaryText);
        Assert.Equal("Cancel", backup.CloseText);
        Assert.Equal("Remove 2 backed-up consultations from ClinicAVT?", backup.RemoveLine);
        Assert.False(backup.DeleteReflections);  // off unless ticked

        await backup.PrimaryCommand.ExecuteAsync(null);
        var removed = engine.Sent("session/remove");
        Assert.Equal(BackedUpIds, removed.GetProperty("ids").EnumerateArray().Select(i => i.GetString()));
        Assert.False(removed.GetProperty("deleteReflections").GetBoolean());
        Assert.Equal("2 consultations removed from this computer.", backup.DoneLine);
        Assert.True(backup.RemovedAny);
    }

    [Fact]
    public async Task APasswordIsCheckedBeforeTheBackupStarts()
    {
        var engine = new FakeEngineClient();
        engine.Responses["archive/summary"] = new { consultations = 4, reflections = 0, unfinished = 0, uncovered = 4 };
        var preferences = TempPreferences();
        preferences.LastBackup = new LastBackup("", "", "2026-08-31T17:00:00Z", 40);
        using var backup = new BackupViewModel(new EngineApi(engine), new FakeFilePicker(),
            new FakeLauncher(), preferences, new InlineDispatcher(), FakeTimeProvider.London());
        backup.PeriodIndex = 1;

        Assert.Equal("Last month (August)", backup.PeriodOptions[1]);
        Assert.Equal("4 consultations, 1 Aug to 31 Aug 2026.", backup.CountLine);

        backup.Password = "short";
        Assert.Equal("Use 8 characters or more.", backup.PasswordProblem);
        backup.Password = "Password1";
        Assert.Contains("too easy to guess", backup.PasswordProblem);
        backup.Password = "harbour lights at dusk";
        backup.PasswordAgain = "harbour lights at dawn";
        Assert.Equal("The two passwords do not match.", backup.PasswordProblem);
        Assert.False(backup.CanStart);
        backup.PasswordAgain = "harbour lights at dusk";
        Assert.True(backup.CanStart);

        // August in London starts at 23:00 UTC the day before
        var picker = new FakeFilePicker { SavePath = @"E:\b.clinicavt" };
        using var saving = new BackupViewModel(new EngineApi(engine), picker,
            new FakeLauncher(), preferences, new InlineDispatcher(), FakeTimeProvider.London())
        { PeriodIndex = 1, Password = "harbour lights at dusk", PasswordAgain = "harbour lights at dusk" };
        await saving.PrimaryCommand.ExecuteAsync(null);
        var sent = engine.Sent("archive/backup");
        Assert.Equal("2026-07-31T23:00:00Z", sent.GetProperty("from").GetString());
        Assert.Equal("2026-08-31T23:00:00Z", sent.GetProperty("to").GetString());
        Assert.Equal(["ClinicAVT backup 1 Aug to 31 Aug 2026"], picker.SuggestedNames);
        Assert.Equal("", saving.OneDriveLine);
    }

    [Fact]
    public async Task FailuresComeBackInPlainWordsAndLeaveTheDialogReadyToTryAgain()
    {
        var engine = new FakeEngineClient();
        engine.Responses["archive/summary"] = new { consultations = 3, reflections = 0, unfinished = 0, uncovered = 3 };
        using var backup = new BackupViewModel(new EngineApi(engine),
            new FakeFilePicker { SavePath = @"E:\b.clinicavt" }, new FakeLauncher(),
            dispatcher: new InlineDispatcher(), clock: FakeTimeProvider.London())
        { Password = "harbour lights", PasswordAgain = "harbour lights" };
        await backup.LoadAsync();

        engine.FailNext = method => method == "archive/backup"
            ? new EngineErrorException(-32001, "Session error", Params("a session is running"))
            : null;
        await backup.PrimaryCommand.ExecuteAsync(null);
        Assert.Equal(BackupStep.Setup, backup.Step);
        Assert.Contains("a consultation is running", backup.Error);
        Assert.DoesNotContain("ession", backup.Error);

        await backup.PrimaryCommand.ExecuteAsync(null);
        engine.RaiseNotification("archive/failed", Params(new { job = "backup", code = "write-failed" }));
        Assert.Equal(BackupStep.Setup, backup.Step);
        Assert.StartsWith("The backup could not be saved there.", backup.Error);
        Assert.EndsWith("Nothing on this computer has changed.", backup.Error);

        // Every fixed code has its own words, and none of them uses the engine's vocabulary
        foreach (var code in new[] { "wrong-password", "not-a-backup", "newer-version", "damaged", "weak-password",
                     "write-failed", "read-failed", "unknown" })
        {
            foreach (var job in new[] { "backup", "restore" })
            {
                var words = BackupWords.Failure(job, code);
                Assert.DoesNotMatch("(?i)session|archive|passphrase|-", words.Replace("ClinicAVT", ""));
            }
        }

        // The status line gets the engine's reason too, reworded where it names a store id
        Assert.Equal("that consultation is no longer on this computer", EngineWords.Reason(
            new EngineErrorException(-32001, "Session error", Params("no session 9f3a"))));
    }

    [Fact]
    public async Task RestoreShowsWhatTheFileHoldsThenAddsOnlyWhatIsNotHere()
    {
        var engine = new FakeEngineClient();
        var picker = new FakeFilePicker { OpenPath = @"E:\ClinicAVT backup 1 Jul to 30 Sep 2026.clinicavt" };
        var preferences = TempPreferences();
        using var restore = new RestoreViewModel(new EngineApi(engine), picker, preferences,
            new InlineDispatcher(), FakeTimeProvider.London());

        // Restoring onto a computer set to keep nothing would override that choice
        await restore.ChooseFileCommand.ExecuteAsync(null);
        restore.Password = "maple-orbit-fender-quill-harbor";
        Assert.True(restore.KeepingOff);
        Assert.False(restore.PrimaryEnabled);
        preferences.KeepConsultations = true;
        using var allowed = new RestoreViewModel(new EngineApi(engine), picker, preferences,
            new InlineDispatcher(), FakeTimeProvider.London());
        await allowed.ChooseFileCommand.ExecuteAsync(null);
        allowed.Password = "maple-orbit-fender-quill-harbor";
        Assert.Equal("ClinicAVT backup 1 Jul to 30 Sep 2026.clinicavt", allowed.FileName);
        Assert.True(allowed.PrimaryEnabled);

        await allowed.PrimaryCommand.ExecuteAsync(null);
        Assert.True(engine.Sent("archive/restore").GetProperty("dryRun").GetBoolean());
        engine.RaiseNotification("archive/failed", Params(new { job = "restore", code = "wrong-password" }));
        Assert.Equal("The password does not match this backup.", allowed.Error);
        Assert.Equal(RestoreStep.Choose, allowed.Step);

        await allowed.PrimaryCommand.ExecuteAsync(null);
        engine.RaiseNotification("archive/done", Params(new
        {
            job = "restore",
            dryRun = true,
            consultations = 33,
            reflections = 10,
            skipped = 5,
            from = "2026-06-30T23:00:00Z",
            to = "2026-09-30T23:00:00Z",
            createdAt = "2026-09-30T17:42:10Z",
            ids = Array.Empty<string>(),
        }));
        Assert.Equal("33 consultations to restore, 1 Jul to 30 Sep 2026.", allowed.SummaryLine);
        Assert.Equal("Restore", allowed.PrimaryText);

        await allowed.PrimaryCommand.ExecuteAsync(null);
        Assert.False(engine.Sent("archive/restore").GetProperty("dryRun").GetBoolean());
        engine.RaiseNotification("archive/progress", Params(new { job = "restore", phase = "writing", done = 3, total = 33 }));
        Assert.Equal("Restoring 3 of 33 consultations…", allowed.ProgressText);
        engine.RaiseNotification("archive/done", Params(new
        {
            job = "restore",
            dryRun = false,
            consultations = 33,
            reflections = 10,
            skipped = 5,
            ids = Array.Empty<string>(),
        }));
        Assert.Equal("33 consultations restored.", allowed.DoneLine);

        // A backup already restored has nothing to add
        using var again = new RestoreViewModel(new EngineApi(engine), picker, preferences,
            new InlineDispatcher(), FakeTimeProvider.London());
        await again.ChooseFileCommand.ExecuteAsync(null);
        again.Password = "maple-orbit-fender-quill-harbor";
        await again.PrimaryCommand.ExecuteAsync(null);
        engine.RaiseNotification("archive/done", Params(new
        {
            job = "restore",
            dryRun = true,
            consultations = 0,
            reflections = 0,
            skipped = 38,
            ids = Array.Empty<string>(),
        }));
        Assert.Equal("Everything in this backup is already in ClinicAVT.", again.SummaryLine);
        Assert.Equal("Close", again.CloseText);
        Assert.False(again.PrimaryEnabled);
        Assert.True(allowed.RestoredAny);
    }
}
