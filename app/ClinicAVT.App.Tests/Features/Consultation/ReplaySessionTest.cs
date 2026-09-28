using System.Diagnostics;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using ClinicAVT.Client;
using static ClinicAVT.App.Tests.Support.Waits;

namespace ClinicAVT.App.Tests.Features.Consultation;

/// <summary>
/// Replays through the whole shell stack against the real engine and real
/// models.
/// </summary>
[Collection("engine")]
[Trait("Requires", "Engine")]
public class ReplaySessionTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(15);

    [Fact]
    public async Task PlayAfterIdleReplaysAndFinalises()
    {
        var wav = SilenceWav.Write(seconds: 5);
        try
        {
            await using var engine = await RealEngine.StartAsync("replay");
            var (host, connection) = (engine.Host, engine.Connection);
            var pidAtStart = host.EnginePid;

            // The connection must survive the idle window between launch and play
            await Task.Delay(TimeSpan.FromSeconds(20));
            Assert.Equal(pidAtStart, host.EnginePid);

            // A silent wav has no transcript, so the real note lane refuses
            // or fails. Any of the three proves the pipeline answered
            var noteDone = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            connection.NotificationReceived += (method, _) =>
            {
                if (method is "note/ready" or "note/failed" or "note/refused")
                {
                    noteDone.TrySetResult();
                }
            };

            await connection.RequestAsync(
                "session/start",
                new { replay = new { path = wav, speed = 1.0, monitor = false } }, Timeout);
            await Task.Delay(TimeSpan.FromSeconds(2));
            await connection.RequestAsync("session/pause", new { paused = true }, Timeout);
            await connection.RequestAsync("session/pause", new { paused = false }, Timeout);
            await connection.RequestAsync("session/stop", null, TimeSpan.FromSeconds(60));
            await noteDone.Task.WaitAsync(TimeSpan.FromSeconds(120));
        }
        finally
        {
            try
            {
                File.Delete(wav);
            }
            catch (IOException)
            {
                // The engine may still hold the wav for a moment, and temp cleans itself
            }
        }
    }

    [Fact]
    public async Task ARealTrackFinalisesToALabelledTranscript()
    {
        var track = Track();
        if (!File.Exists(track))
        {
            return;  // demo tracks not staged on this machine
        }

        await using var engine = await RealEngine.StartAsync("track", stderr: "clinicavt-track-test.log");
        var connection = engine.Connection;

        var noteReady = new TaskCompletionSource<string>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var patientReady = new TaskCompletionSource<string>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var partials = 0;
        connection.NotificationReceived += (method, parameters) =>
        {
            switch (method)
            {
                case "note/partial":
                    partials++;
                    break;
                case "note/ready":
                    noteReady.TrySetResult(parameters.GetProperty("text").GetString() ?? "");
                    break;
                case "note/failed":
                    noteReady.TrySetException(new InvalidOperationException(
                        parameters.GetProperty("detail").GetString()));
                    break;
                case "patient/ready":
                    patientReady.TrySetResult(
                        parameters.GetProperty("text").GetString() ?? "");
                    break;
                case "patient/failed":
                    patientReady.TrySetException(new InvalidOperationException(
                        parameters.GetProperty("detail").GetString()));
                    break;
                default:
                    break;
            }
        };

        await connection.RequestAsync(
            "session/start",
            new { replay = new { path = track, speed = 16.0, monitor = false } }, Timeout);
        await Task.Delay(TimeSpan.FromSeconds(20));  // ~5 min of audio at 16x
        var stop = await connection.RequestAsync(
            "session/stop", null, TimeSpan.FromSeconds(240));

        var id = stop.GetProperty("sessionId").GetString();
        var transcript = await connection.RequestAsync(
            "session/transcript", new { id }, Timeout);
        var labelled = transcript.GetProperty("turns").EnumerateArray()
            .Count(t => t.GetProperty("speaker").GetString() is "doctor" or "patient");
        Assert.True(labelled > 5, $"expected a labelled transcript, got {labelled} labelled turns");

        // The real note follows, streamed then stored
        var note = await noteReady.Task.WaitAsync(TimeSpan.FromSeconds(300));
        Assert.False(string.IsNullOrWhiteSpace(note));
        Assert.True(partials > 3, $"the note must stream, saw {partials} partials");
        var stored = await connection.RequestAsync("session/note", new { id }, Timeout);
        Assert.Equal(note, stored.GetProperty("text").GetString());

        // The patient information follows the note
        var patient = await patientReady.Task.WaitAsync(TimeSpan.FromSeconds(300));
        Assert.Contains("Your appointment today", patient);
        var storedPatient =
            await connection.RequestAsync("session/patient", new { id }, Timeout);
        Assert.Equal(patient, storedPatient.GetProperty("text").GetString());
    }

    [Fact]
    public async Task AKilledEngineResumesTheSessionAndTheConsultSurvives()
    {
        var track = Track();
        if (!File.Exists(track))
        {
            return;  // demo tracks not staged on this machine
        }

        await using var engine = await RealEngine.StartAsync("resume", stderr: "clinicavt-resume-test.log");
        var (host, connection) = (engine.Host, engine.Connection);

        var noteReady = new TaskCompletionSource<string>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        connection.NotificationReceived += (method, parameters) =>
        {
            switch (method)
            {
                case "note/ready":
                    noteReady.TrySetResult(parameters.GetProperty("text").GetString() ?? "");
                    break;
                case "note/failed":
                    noteReady.TrySetException(new InvalidOperationException(
                        parameters.GetProperty("detail").GetString()));
                    break;
                default:
                    break;
            }
        };

        var started = await connection.RequestAsync(
            "session/start",
            new { replay = new { path = track, speed = 16.0, monitor = false } }, Timeout);
        var firstId = started.GetProperty("sessionId").GetString();

        // Mid-consult, the engine dies the way the driver fault kills it
        await Task.Delay(TimeSpan.FromSeconds(8));
        Process.GetProcessById(host.EnginePid!.Value).Kill();
        await RetryAsync(() => connection.RequestAsync("engine/echo", new { payload = "back" }, Timeout));

        // Resume as the shell does. Stored audio replays ahead of the rest of the file
        var resumed = await connection.RequestAsync(
            "session/start",
            new
            {
                resume = firstId,
                replay = new { path = track, speed = 16.0, monitor = false },
            }, TimeSpan.FromSeconds(60));
        var secondId = resumed.GetProperty("sessionId").GetString();
        Assert.NotEqual(firstId, secondId);

        await Task.Delay(TimeSpan.FromSeconds(14));
        var stop = await connection.RequestAsync("session/stop", null, TimeSpan.FromSeconds(240));
        Assert.Equal(secondId, stop.GetProperty("sessionId").GetString());

        // The transcript covers the whole consult, including the audio before the kill
        var transcript = await connection.RequestAsync(
            "session/transcript", new { id = secondId }, Timeout);
        var labelled = transcript.GetProperty("turns").EnumerateArray()
            .Count(t => t.GetProperty("speaker").GetString() is "doctor" or "patient");
        Assert.True(labelled > 5, $"expected a full labelled transcript, got {labelled} turns");

        var note = await noteReady.Task.WaitAsync(TimeSpan.FromSeconds(300));
        Assert.False(string.IsNullOrWhiteSpace(note));
    }

    [Fact]
    public async Task LiveCountersSurviveAKilledEngineAndResume()
    {
        var track = Track();
        if (!File.Exists(track))
        {
            return;
        }

        await using var engine = await RealEngine.StartAsync("counters", stderr: "clinicavt-counters-test.log");
        var (host, connection) = (engine.Host, engine.Connection);
        var session = new ConsultationViewModel(new EngineApi(connection), new InlineDispatcher(), new TranscriptViewModel(), new NoteViewModel(), TestSession.Status(), new FakeDialogService(), TestSession.Page(connection, TestSession.Status()), TestSession.Guidance(TestSession.Status()));

        await session.StartRecordingAsync(new ReplayRequest(track, 16.0, false));
        Assert.Equal(SessionState.Recording, session.State);
        await WaitUntilAsync(() => session.AudioSeconds > 3, TimeSpan.FromSeconds(20));

        // Two kills in quick succession with the second mid-resume, matching
        // a real double crash
        var atKill = session.AudioSeconds;
        var firstPid = host.EnginePid!.Value;
        Process.GetProcessById(firstPid).Kill();
        await WaitUntilAsync(
            () => host.EnginePid is int pid && pid != firstPid, TimeSpan.FromSeconds(30));
        Process.GetProcessById(host.EnginePid!.Value).Kill();

        // The supervisor restarts again, the resume retries, and the
        // counters must keep counting from where they were
        await WaitUntilAsync(
            () => session.AudioSeconds > atKill + 30, TimeSpan.FromSeconds(90));
        Assert.Equal(SessionState.Recording, session.State);

        await session.StopRecordingAsync();
        Assert.True(
            session.State is SessionState.Finalising or SessionState.Review,
            $"stop left the session in {session.State}");
    }

    [Fact]
    [Trait("Requires", "CrashBattery")]
    public async Task AcceleratedSessionStartsSurviveTheWorkerLoad()
    {
        var track = Track();
        if (!File.Exists(track))
        {
            return;
        }

        // A cold compile cache is the hard case. The 9B compiles on the GPU
        // while 16x whisper floods it
        var noteCache = Path.Combine(EnginePath.FindModels()!, "qwen3.5-9b-int4", ".cache");
        var crashes = 0;
        for (var round = 0; round < 6; round++)
        {
            if (Directory.Exists(noteCache))
            {
                try
                {
                    Directory.Delete(noteCache, recursive: true);
                }
                catch (IOException)
                {
                }
            }

            await using var engine = await RealEngine.StartAsync("battery", stderr: "clinicavt-battery-test.log");
            var connection = engine.Connection;
            await connection.RequestAsync(
                "session/start",
                new { replay = new { path = track, speed = 16.0, monitor = false } }, Timeout);
            await Task.Delay(TimeSpan.FromSeconds(12));
            if (File.Exists(engine.CrashLog) && File.ReadAllLines(engine.CrashLog).Length > 0)
            {
                crashes++;
            }
            else
            {
                await connection.RequestAsync("session/cancel", null, TimeSpan.FromSeconds(30));
            }
        }

        Assert.True(crashes == 0, $"{crashes}/6 accelerated session starts crashed the engine");
    }

    private static string Track() => Path.Combine(
        Path.GetDirectoryName(EnginePath.FindModels()) ?? "", "demo", "day2_consultation02_mixed.wav");
}
