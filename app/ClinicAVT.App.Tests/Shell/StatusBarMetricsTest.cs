using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;
using static ClinicAVT.App.Tests.Support.Waits;
using static ClinicAVT.App.Tests.Support.Wire;

namespace ClinicAVT.App.Tests.Shell;

/// <summary>The live numbers behind the status bar's model chips.</summary>
public class StatusBarMetricsTest
{
    private static (TestShell Shell, ModelChips Status, FakeEngineClient Engine) Create()
    {
        var engine = new FakeEngineClient(autoNotify: false);
        var shell = new TestShell(engine);
        return (shell, shell.Chips, engine);
    }

    // A load behind a ready app stays off the status line: nothing waits on it. The time is
    // counted for the places that do wait, and a switch's idle step cannot end the count. A first
    // move to the NPU compiles for minutes, so its line counts the time and never looks hung
    [Fact]
    public void AModelLoadAndADeviceMoveCountTheirTimeUntilTheyEnd()
    {
        var engine = new FakeEngineClient(autoNotify: false);
        var clock = new FakeTimeProvider();
        using var shell = new TestShell(engine, time: clock);
        var status = shell.Status;
        var models = shell.Models;
        shell.Host.RaiseStatus(EngineStatus.Running);

        engine.RaiseNotification("note/model", Params(new { tier = "accuracy", state = "loading" }));
        // A switch announces idle between the old model and the new: no flash of Ready
        engine.RaiseNotification("note/model", Params(new { tier = "accuracy", state = "idle" }));
        engine.RaiseNotification("note/model", Params(new { tier = "accuracy", state = "loading" }));
        clock.Advance(TimeSpan.FromSeconds(48));

        Assert.True(models.ModelLoading);
        Assert.Equal("Loading the note model · 0:48 · this can take a few minutes", models.ModelLoadLine);
        Assert.Equal("Ready", status.DisplayLabel);
        Assert.False(status.ShowsSetup);

        engine.RaiseNotification("note/model", Params(new { tier = "accuracy", state = "ready" }));
        Assert.False(models.ModelLoading);
        Assert.Equal("Ready", status.DisplayLabel);

        models.BeginSwitch("Switching to the NPU · {time} · first time may take longer");
        Assert.Equal("Switching to the NPU · 0:00 · first time may take longer", status.DisplayLabel);
        Assert.False(status.ConsentVisible, "no consent reminder over a wait");
        clock.Advance(TimeSpan.FromSeconds(72));
        Assert.Equal("Switching to the NPU · 1:12 · first time may take longer", status.DisplayLabel);
        Assert.True(status.Busy);

        models.EndSwitch();
        Assert.True(status.ConsentVisible);
        shell.Line.Append("Ready");
        clock.Advance(TimeSpan.FromSeconds(5));
        Assert.Equal("Ready", status.DisplayLabel);
    }

    [Fact]
    public async Task TheNoteChipThroughAGenerationAndAModelLoad()
    {
        var (shell, status, engine) = Create();
        await WaitUntilAsync(() => status.NoteChip.Length > 0);
        Assert.Equal("Qwen3.5 9B · GPU", status.NoteChip);
        Assert.False(status.TokensStreaming);
        Assert.False(status.NoteActive);

        // The engine meters at the source, before its 12 Hz throttle, and the
        // shell shows that figure
        engine.RaiseNotification("note/partial",
            Params(new { text = "The", tokensPerSecond = 15.3 }));
        Assert.True(status.TokensStreaming);
        Assert.True(status.NoteActive);
        Assert.Equal(15.3, status.TokensPerSecond);
        Assert.Contains("15.3 tok/s", status.NoteChip);

        // The ready event carries the whole generation's average, which
        // holds and is labelled as what it is
        engine.RaiseNotification("note/ready",
            Params(new { text = "The note.", tokensPerSecond = 14.2 }));
        Assert.Equal(14.2, status.TokensPerSecond);
        Assert.False(status.TokensStreaming, "the stream ended and the value holds");
        Assert.False(status.NoteActive);
        Assert.Contains("Averaged 14.2 tok/s", status.NoteChip);

        // The translator is another model: its rate never reaches the note chip
        engine.RaiseNotification("translate/partial", Params(new { text = "Twoja", tokensPerSecond = 95.2 }));
        engine.RaiseNotification("translate/ready", Params(new { text = "Twoja notatka.", language = "Polish", tokensPerSecond = 98.1 }));
        Assert.Equal(14.2, status.TokensPerSecond);
        Assert.False(status.TokensStreaming);
        Assert.Contains("Averaged 14.2 tok/s", status.NoteChip);

        // A failure ends the stream too
        engine.RaiseNotification("note/partial", Params(new { text = "The" }));
        Assert.True(status.TokensStreaming);
        engine.RaiseNotification("note/failed");
        Assert.False(status.TokensStreaming);

        // A new consultation clears the frozen value
        shell.Activity.Start();
        Assert.Equal("Qwen3.5 9B · GPU", status.NoteChip);
        Assert.False(status.TokensStreaming);
        Assert.Equal(0, status.TokensPerSecond);

        // At start the model list is fetched before the shell sends its saved tier, so it names
        // the engine's default. The lane's own messages name the model being loaded
        engine.RaiseNotification("note/model", Params(new { tier = "constrained", name = "Qwen3.5 4B", state = "loading" }));
        Assert.StartsWith("Qwen3.5 4B", status.NoteChip);
        var fetched = engine.Requests.Count(r => r.Method == "engine/models");
        engine.RaiseNotification("note/model", Params(new { tier = "constrained", name = "Qwen3.5 4B", state = "ready" }));
        await WaitUntilAsync(() => engine.Requests.Count(r => r.Method == "engine/models") > fetched);
        Assert.StartsWith("Qwen3.5 4B", status.NoteChip);

        // A tier switch renames the chip even when the engine is too busy to
        // answer the store call
        engine.Failing.Add("engine/models");
        engine.RaiseNotification("note/model", Params(new
        {
            state = "ready",
            tier = "accuracy",
            id = "qwen3.6-35b-a3b-int4",
            name = "Qwen3.6 35B",
            seconds = 62.0,
        }));
        Assert.Equal("Qwen3.6 35B · GPU", status.NoteChip);
    }

    [Fact]
    public async Task TheAsrAndMemoryChipsThroughAConsultation()
    {
        var (shell, status, engine) = Create();
        var memory = (FakeProcessMetrics)shell.Get<IProcessMetrics>();
        memory.Committed = 5.06;
        await WaitUntilAsync(() => status.AsrChip.Length > 0);  // the connect-time model fetch
        Assert.Equal("Whisper Large v3 Turbo · GPU", status.AsrChip);
        Assert.Equal("Qwen3.5 9B · GPU", status.NoteChip);
        Assert.False(status.AsrActive);

        // While idle the poll reads the engine's figure
        await status.PollMetricsOnceAsync();
        Assert.Equal(33.4, status.RealtimeFactor);  // FakeEngineClient's figure
        Assert.Equal("Memory · 5.1 GB", status.MemoryChip);

        // While recording the realtime factor joins Whisper's chip and the dot lights
        shell.Activity.Listening = true;
        Assert.True(status.AsrActive);
        await status.PollMetricsOnceAsync();
        Assert.Equal("Whisper Large v3 Turbo · GPU · 33× RT", status.AsrChip);

        // A slow factor keeps a decimal
        engine.MetricsRealtimeFactor = 1.4;
        await status.PollMetricsOnceAsync();
        Assert.Equal("Whisper Large v3 Turbo · GPU · 1.4× RT", status.AsrChip);

        // After stop the tail still decodes, the NPU's longest stage, so the
        // figure and the dot stay until the transcript seals
        shell.Activity.StopListening();
        shell.Activity.Decoding = true;
        Assert.True(status.AsrActive);
        Assert.Equal("Whisper Large v3 Turbo · GPU · 1.4× RT", status.AsrChip);

        // Once sealed the session's average holds, labelled as an average
        shell.Activity.Decoding = false;
        Assert.False(status.AsrActive, "sealed: the dot rests while Averaged shows");
        Assert.Equal("Whisper Large v3 Turbo · GPU · Averaged 1.4× RT", status.AsrChip);

        shell.Activity.Start();  // the next consultation starts clean
        Assert.Equal("Whisper Large v3 Turbo · GPU", status.AsrChip);

        memory.Committed = 0;  // the memory provider failed, so no figure shows
        await status.PollMetricsOnceAsync();
        Assert.Equal("", status.MemoryChip);
    }
}
