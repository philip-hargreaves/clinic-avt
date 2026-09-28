using System.Text.Json;
using ClinicAVT.App.Core.Metrics;

namespace ClinicAVT.App.Tests.Metrics;

public class ReportBuilderTest
{
    private static readonly MachineInfo Machine = new(
        "Intel(R) Core(TM) Ultra 7 258V", 32, "Windows 11 26200",
        [new GpuInfo("Intel(R) Arc(TM) 140T GPU", "32.0.101.6083")],
        new GpuInfo("Intel(R) AI Boost", "32.0.100.3104"));

    private static string Session(string asr, double audioSeconds = 541.5,
        string model = "Qwen3.5 9B", string start = "2026-08-19T21:12:44Z") =>
        JsonSerializer.Serialize(new
        {
            schema = 2,
            start,
            outcome = "completed",
            engine = new
            {
                devices = new { asr },
                asrRealtimeFactor = 33.4,
                stageSeconds = new Dictionary<string, double> { ["transcript sealed"] = 2.0 },
                loadSeconds = new { asr = 2.1 },
                audioSeconds,
            },
            note = new
            {
                model,
                tier = "default",
                modelLoadSeconds = 24.1,
                firstPartialAfterStopSeconds = 3.5,
                readyAfterStopSeconds = 9.1,
                tokensPerSecond = 17.4,
                chars = 1290,
            },
            patient = new { firstPartialAfterNoteSeconds = 1.2, readyAfterNoteSeconds = 12.8, tokensPerSecond = 16.0 },
            memory = new { peakWorkingSetMb = 9800, availableAtStartMb = 21000, noteHostPeakWorkingSetMb = 7782 },
        });

    [Fact]
    public void RendersMachineStepsTotalsAndEmbeddedJsonAndSkipsAGarbledLine()
    {
        var html = ReportBuilder.Build(Machine,
            ["not json at all", Session("GPU.0")], DateTimeOffset.UtcNow);

        Assert.Contains("Intel(R) Core(TM) Ultra 7 258V", html);
        Assert.Contains("Qwen3.5 9B", html);
        // Transcript 2.0, waiting 1.5, clinical note 5.6, on screen 9.1, patient 12.8, both 21.9
        var consultations = html.Split("<h2>Consultations</h2>")[1];
        Assert.Contains("<td class=\"n\">1.5</td>", consultations);
        Assert.Contains("<td class=\"n\">5.6</td>", consultations);
        Assert.Contains("<td class=\"t\">9.1</td>", consultations);
        Assert.Contains("<td class=\"t\">21.9</td>", consultations);
        Assert.Contains("Clinical note on screen", html);
        Assert.Contains("Both notes on screen", html);
        Assert.Contains("1 consultation,", html);
        var json = html.Split("type=\"application/json\">")[1].Split("</script>")[0];
        using var embedded = JsonDocument.Parse(json);
        Assert.Equal(1, embedded.RootElement.GetProperty("sessions").GetArrayLength());
    }

    [Fact]
    public void SummaryGroupsByNoteModelAndLeavesOutShortRecordings()
    {
        var html = ReportBuilder.Build(Machine,
        [
            Session("GPU.0", model: "Qwen3.5 9B", start: "2026-08-19T21:12:44Z"),
            Session("GPU.0", model: "Qwen3.5 9B", start: "2026-08-19T22:12:44Z"),
            Session("GPU.0", model: "Qwen3.6 35B", start: "2026-08-19T23:12:44Z"),
            Session("NPU", audioSeconds: 4),
        ], DateTimeOffset.UtcNow);

        Assert.Contains("3 consultations, 1 short recording", html);
        var summary = html.Split("<h2>Summary</h2>")[1].Split("<h2>Consultations</h2>")[0];
        Assert.Contains("Qwen3.5 9B · GPU", summary);
        Assert.Contains("2 consultations", summary);
        Assert.Contains("Qwen3.6 35B · GPU", summary);
        Assert.Contains("21.9<span class=\"w\"> · 21.9</span>", summary);  // median · slowest
        Assert.Contains("Peak memory of the note model, GB", summary);
        Assert.Contains("7.6", summary);
        Assert.DoesNotContain("NPU", summary);
        Assert.Contains("Recordings under 30 seconds (1)", html);
    }

    [Fact]
    public void AnOlderRecordWithoutThePatientNoteRendersWithBlanks()
    {
        var session = JsonSerializer.Serialize(new
        {
            schema = 1,
            start = "2026-08-19T21:12:44Z",
            source = "mic",
            engine = new
            {
                devices = new { asr = "GPU.0" },
                stageSeconds = new Dictionary<string, double> { ["transcript sealed"] = 2.0 },
                audioSeconds = 300.0,
            },
            note = new { firstPartialAfterStopSeconds = 15.1, readyAfterStopSeconds = 31.3, chars = 1290 },
            memory = new { peakWorkingSetMb = 9800 },
        });

        var html = ReportBuilder.Build(Machine, [session], DateTimeOffset.UtcNow);

        Assert.Contains("Note model not recorded", html);
        Assert.Contains("<td class=\"t\">31.3</td>", html);
        Assert.Contains("<td class=\"t\">–</td>", html);
    }

    [Fact]
    public void RendersPowerModeAndEngineThrottling()
    {
        Assert.Equal("efficiency", PowerState.ModeName("961CC777-2547-4F9D-8174-7D86181B8A7A"));
        Assert.Equal("performance", PowerState.ModeName("ded574b5-45a0-4f42-8737-46345c09c238"));
        Assert.Equal("balanced", PowerState.ModeName(""));
        Assert.Equal("balanced", PowerState.ModeName("00000000-0000-0000-0000-000000000000"));
        Assert.Equal("unknown", PowerState.ModeName("not-a-guid"));

        var session = JsonSerializer.Serialize(new
        {
            start = "2026-08-29T13:40:17Z",
            engine = new { devices = new { asr = "GPU.0" }, powerThrottling = "off" },
            power = new { mode = "performance", onMains = true },
            note = new { chars = 500 },
            memory = new { },
        });

        var html = ReportBuilder.Build(Machine, [session], DateTimeOffset.UtcNow);

        // The separator is HTML-encoded, so assert the three facts
        var machine = html.Split("<h2>Machine</h2>")[1].Split("</table>")[0];
        Assert.Contains("performance mode", machine);
        Assert.Contains("mains", machine);
        Assert.Contains("engine throttling off", machine);
    }
}
