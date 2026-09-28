using System.Globalization;
using System.Net;
using System.Text;
using System.Text.Json;
using ClinicAVT.App.Core.Common;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Metrics;

/// <summary>
/// Renders metrics.jsonl into one self-contained HTML file. It shows how long each step after
/// Stop took, per note model, and embeds the raw JSON for scripts.
/// </summary>
public static class ReportBuilder
{
    // Shorter than this is treated as an accidental click
    private const double MinAudioSeconds = 30;

    private sealed record Consultation(
        JsonElement Source,
        string Started,
        string Length,
        string Device,
        string Model,
        double? Transcript,
        double? Waiting,
        double? Clinical,
        double? ClinicalOnScreen,
        double? Patient,
        double? BothOnScreen,
        double? TokensPerSecond,
        double? ModelLoad,
        double? PeakGb)
    {
        public bool Short => Source.Number("engine", "audioSeconds") is { } s && s < MinAudioSeconds;
    }

    public static string Build(MachineInfo machine, IReadOnlyList<string> jsonlLines,
        DateTimeOffset exported)
    {
        var sessions = new List<JsonElement>();
        foreach (var line in jsonlLines)
        {
            try
            {
                sessions.Add(JsonDocument.Parse(line).RootElement.Clone());
            }
            catch (JsonException)
            {
            }
        }

        var rows = sessions.Select(ToRow).ToList();
        var consultations = rows.Where(r => !r.Short).ToList();
        var shorts = rows.Where(r => r.Short).ToList();

        var html = new StringBuilder();
        html.Append("<!doctype html><html><head><meta charset=\"utf-8\">");
        html.Append("<title>ClinicAVT Performance Report</title><style>");
        html.Append("body{font-family:Segoe UI,sans-serif;max-width:74rem;margin:2rem auto;");
        html.Append("padding:0 1rem;color:#1a1a1a}h1{font-size:1.5rem;margin-bottom:.2rem}");
        html.Append("h2{font-size:1.1rem;margin:2rem 0 .2rem}.sub{color:#666;font-size:.9rem;margin:.1rem 0}");
        html.Append("p.hint{font-size:.85rem;color:#555;margin:.3rem 0 0}");
        html.Append("table{border-collapse:collapse;width:100%;font-size:.88rem;margin-top:.6rem}");
        html.Append("th,td{text-align:left;padding:.35rem .6rem;border-bottom:1px solid #e3e3e3;white-space:nowrap}");
        html.Append("th{color:#555;font-weight:600}td.n,th.n{text-align:right;font-variant-numeric:tabular-nums}");
        html.Append("td.t,th.t{text-align:right;font-weight:600;border-left:1px solid #e3e3e3}");
        html.Append("tr.tot td{font-weight:600;background:#f7f7f7}tr.section th{padding-top:.9rem;color:#333}");
        html.Append(".w{color:#888;font-weight:400}");
        html.Append(".bar{display:flex;height:10px;width:200px;border-radius:2px;overflow:hidden;background:#eee}");
        html.Append(".bar span{display:block;height:100%}");
        html.Append(".c1{background:#8fa3b8}.c2{background:#d9b44a}.c3{background:#4a7fb5}.c4{background:#7fb57d}");
        html.Append(".legend{font-size:.85rem;color:#555;margin:.4rem 0 0}");
        html.Append(".legend i{display:inline-block;width:10px;height:10px;margin:0 .3rem 0 1rem;vertical-align:middle;border-radius:2px}");
        html.Append("details{margin-top:.8rem}summary{cursor:pointer;color:#444}");
        html.Append("pre{background:#f6f6f6;padding:1rem;overflow-x:auto;font-size:.8rem}</style></head><body>");
        html.Append("<h1>ClinicAVT Performance Report</h1>");
        html.Append(CultureInfo.InvariantCulture,
            $"<p class=\"sub\">Exported {exported:yyyy-MM-dd HH:mm} UTC · "
            + $"{Words.Count(consultations.Count, "consultation")}, "
            + $"{Words.Count(shorts.Count, "short recording")}</p>");

        AppendMachine(html, machine, sessions);
        AppendSummary(html, consultations);
        AppendConsultations(html, consultations);
        AppendShorts(html, shorts);

        html.Append("<h2>Raw data</h2><details><summary>Session records (JSON)</summary><pre>");
        foreach (var session in sessions)
        {
            html.Append(WebUtility.HtmlEncode(session.GetRawText())).Append('\n');
        }

        html.Append("</pre></details>");
        html.Append("<script id=\"clinicavt-metrics\" type=\"application/json\">");
        html.Append(JsonSerializer.Serialize(new { machine, sessions }));
        html.Append("</script></body></html>");
        return html.ToString();
    }

    private static Consultation ToRow(JsonElement s)
    {
        var transcript = s.Number("engine", "stageSeconds", "transcript sealed");
        var first = s.Number("note", "firstPartialAfterStopSeconds");
        var ready = s.Number("note", "readyAfterStopSeconds");
        var patient = s.Number("patient", "readyAfterNoteSeconds");
        var device = s.Text("engine", "devices", "asr") ?? "";
        var dot = device.IndexOf('.');
        var start = s.Text("start") ?? "";
        return new Consultation(
            s,
            start[..Math.Min(16, start.Length)].Replace('T', ' '),
            Clock(s.Number("engine", "audioSeconds")),
            dot > 0 ? device[..dot] : device,
            s.Text("note", "model") ?? "Note model not recorded",
            transcript,
            Delta(transcript, first),
            Delta(first, ready),
            ready,
            patient,
            ready is null || patient is null ? null : ready + patient,
            s.Number("note", "tokensPerSecond"),
            s.Number("note", "modelLoadSeconds"),
            s.Number("memory", "noteHostPeakWorkingSetMb") is { } mb ? Math.Round(mb / 1024, 1) : null);
    }

    private static void AppendSummary(StringBuilder html, List<Consultation> rows)
    {
        html.Append("<h2>Summary</h2>");
        if (rows.Count == 0)
        {
            html.Append("<p class=\"hint\">No consultations recorded yet.</p>");
            return;
        }

        html.Append("<p class=\"hint\">Consultations longer than 30 seconds. "
                    + "How long each step took after Stop was pressed.</p>");
        var groups = rows.GroupBy(r => (r.Model, r.Device))
            .OrderBy(g => g.Key.Device).ThenBy(g => g.Max(r => r.PeakGb ?? 0)).ToList();
        html.Append("<table><tr><th>Seconds, median <span class=\"w\">· slowest</span></th>");
        foreach (var g in groups)
        {
            html.Append(CultureInfo.InvariantCulture,
                $"<th class=\"n\">{WebUtility.HtmlEncode(g.Key.Model)} · {WebUtility.HtmlEncode(g.Key.Device)}"
                + $"<br><span class=\"w\">{Words.Count(g.Count(), "consultation")}</span></th>");
        }

        html.Append("</tr>");
        Step(html, groups, "Transcript", r => r.Transcript);
        Step(html, groups, "Waiting for the clinical note", r => r.Waiting);
        Step(html, groups, "Clinical note", r => r.Clinical);
        Step(html, groups, "Clinical note on screen", r => r.ClinicalOnScreen, total: true);
        Step(html, groups, "Patient information", r => r.Patient);
        Step(html, groups, "Both notes on screen", r => r.BothOnScreen, total: true);
        html.Append("<tr class=\"section\"><th>Other</th>")
            .Append(string.Concat(Enumerable.Repeat("<th></th>", groups.Count))).Append("</tr>");
        Step(html, groups, "Writing speed, tokens per second", r => r.TokensPerSecond, slowestIsMin: true);
        Step(html, groups, "Note model load when the app starts, s", r => r.ModelLoad);
        Step(html, groups, "Peak memory of the note model, GB", r => r.PeakGb);
        html.Append("</table>");
    }

    private static void Step(StringBuilder html, List<IGrouping<(string Model, string Device), Consultation>> groups,
        string label, Func<Consultation, double?> pick, bool total = false, bool slowestIsMin = false)
    {
        html.Append(total ? "<tr class=\"tot\">" : "<tr>");
        html.Append(CultureInfo.InvariantCulture, $"<td>{WebUtility.HtmlEncode(label)}</td>");
        foreach (var g in groups)
        {
            var values = g.Select(pick).Where(v => v is not null).Select(v => v!.Value).OrderBy(v => v).ToList();
            var cell = values.Count == 0
                ? "–"
                : Format(Median(values)) + "<span class=\"w\"> · "
                  + Format(slowestIsMin ? values[0] : values[^1]) + "</span>";
            html.Append(CultureInfo.InvariantCulture, $"<td class=\"{(total ? "t" : "n")}\">{cell}</td>");
        }

        html.Append("</tr>");
    }

    private static void AppendConsultations(StringBuilder html, List<Consultation> rows)
    {
        html.Append("<h2>Consultations</h2>");
        if (rows.Count == 0)
        {
            html.Append("<p class=\"hint\">None yet.</p>");
            return;
        }

        html.Append("<p class=\"hint\">Seconds each step took after Stop was pressed. "
                    + "The bar is the total, drawn to the same scale on every row.</p>");
        html.Append("<p class=\"legend\"><i class=\"c1\"></i>Transcript <i class=\"c2\"></i>Waiting for the clinical note "
                    + "<i class=\"c3\"></i>Clinical note <i class=\"c4\"></i>Patient information</p>");
        html.Append("<table><tr><th>Started</th><th class=\"n\">Length</th><th>Note model</th><th></th>");
        StepHeaders(html);
        html.Append("</tr>");
        var longest = rows.Max(r => r.BothOnScreen ?? r.ClinicalOnScreen ?? 0);
        foreach (var r in rows.OrderByDescending(r => r.Started))
        {
            html.Append("<tr>");
            Cell(html, r.Started);
            Cell(html, r.Length, "n");
            Cell(html, r.Model);
            html.Append("<td>").Append(Bar(r, longest)).Append("</td>");
            StepCells(html, r);
            html.Append("</tr>");
        }

        html.Append("</table>");
    }

    private static void AppendShorts(StringBuilder html, List<Consultation> rows)
    {
        if (rows.Count == 0)
        {
            return;
        }

        html.Append(CultureInfo.InvariantCulture, $"<details><summary>Recordings under 30 seconds ({rows.Count})</summary>");
        html.Append("<p class=\"hint\">Too short to measure.</p>");
        html.Append("<table><tr><th>Started</th><th class=\"n\">Length</th>");
        html.Append("<th>Device</th><th>Note model</th>");
        StepHeaders(html);
        html.Append("</tr>");
        foreach (var r in rows.OrderByDescending(r => r.Started))
        {
            html.Append("<tr>");
            Cell(html, r.Started);
            Cell(html, r.Length, "n");
            Cell(html, r.Device);
            Cell(html, r.Model);
            StepCells(html, r);
            html.Append("</tr>");
        }

        html.Append("</table></details>");
    }

    private static void StepHeaders(StringBuilder html) =>
        html.Append("<th class=\"n\">Transcript</th><th class=\"n\">Waiting</th><th class=\"n\">Clinical note</th>")
            .Append("<th class=\"t\">Clinical note on screen</th><th class=\"n\">Patient information</th>")
            .Append("<th class=\"t\">Both notes on screen</th>");

    private static void StepCells(StringBuilder html, Consultation r)
    {
        Cell(html, Format(r.Transcript), "n");
        Cell(html, Format(r.Waiting), "n");
        Cell(html, Format(r.Clinical), "n");
        Cell(html, Format(r.ClinicalOnScreen), "t");
        Cell(html, Format(r.Patient), "n");
        Cell(html, Format(r.BothOnScreen), "t");
    }

    private static string Bar(Consultation r, double longest)
    {
        double[] parts = [r.Transcript ?? 0, r.Waiting ?? 0, r.Clinical ?? 0, r.Patient ?? 0];
        var total = parts.Sum();
        if (total <= 0 || longest <= 0)
        {
            return "";
        }

        var width = 200 * total / longest;
        var bar = new StringBuilder();
        bar.Append(CultureInfo.InvariantCulture, $"<div class=\"bar\" style=\"width:{width:0}px\">");
        for (var i = 0; i < parts.Length; i++)
        {
            bar.Append(CultureInfo.InvariantCulture, $"<span class=\"c{i + 1}\" style=\"flex:{parts[i]:0.00}\"></span>");
        }

        return bar.Append("</div>").ToString();
    }

    // Runtime-reported device names, with the matching Windows driver
    // version folded into the same row
    private static void AppendMachine(StringBuilder html, MachineInfo machine,
        List<JsonElement> sessions)
    {
        html.Append("<h2>Machine</h2><table>");
        Row(html, "Processor", $"{machine.Cpu} · {machine.RamGb} GB RAM");
        Row(html, "Windows", machine.Os);
        var drivers = machine.Gpus.ToList();
        if (machine.Npu is not null)
        {
            drivers.Add(machine.Npu);
        }

        var matched = new HashSet<GpuInfo>();
        var hardware = sessions.Select(s => s.Find("engine", "hardware"))
            .LastOrDefault(h => h is { ValueKind: JsonValueKind.Object });
        if (hardware is not null)
        {
            foreach (var device in hardware.Value.EnumerateObject())
            {
                if (device.Name == "CPU")
                {
                    continue;
                }

                var name = device.Name == "NPU"
                    ? NpuGeneration(device.Value.GetString() ?? "")
                    : device.Value.GetString() ?? "";
                var driver = drivers.FirstOrDefault(d => SameDevice(d.Name, name));
                if (driver is not null)
                {
                    matched.Add(driver);
                }

                Row(html, device.Name,
                    driver is null ? name : $"{name} · driver {driver.Driver}");
            }
        }

        foreach (var driver in drivers.Where(d => !matched.Contains(d)))
        {
            Row(html, hardware is null ? "Device" : "Other device",
                $"{driver.Name} · driver {driver.Driver}");
        }

        var openvino = sessions.Select(s => s.Text("engine", "openvino"))
            .LastOrDefault(v => v is not null);
        if (openvino is not null)
        {
            Row(html, "OpenVINO", openvino);
        }

        // Power mode and throttling decide the finalise floor. The last
        // session's state stands for the report
        var power = sessions.Select(s => s.Find("power"))
            .LastOrDefault(p => p is { ValueKind: JsonValueKind.Object });
        var throttling = sessions.Select(s => s.Text("engine", "powerThrottling"))
            .LastOrDefault(v => v is not null);
        if (power is not null || throttling is not null)
        {
            var parts = new List<string>();
            if (power is not null)
            {
                parts.Add($"{power.Value.Text("mode") ?? "?"} mode");
                parts.Add(power.Value.TryGetProperty("onMains", out var mains) && mains.GetBoolean()
                    ? "mains" : "battery");
            }

            if (throttling is not null)
            {
                parts.Add($"engine throttling {throttling}");
            }

            Row(html, "Power", string.Join(" · ", parts));
        }

        html.Append("</table>");
    }

    private static readonly Dictionary<string, string> NpuGenerations = new()
    {
        ["2700"] = "NPU 2",
        ["3720"] = "NPU 3",
        ["4000"] = "NPU 4",
        ["5000"] = "NPU 5",
    };

    // The driver names every Intel NPU "AI Boost". The architecture code identifies the model,
    // so it is translated to the generation
    private static string NpuGeneration(string name)
    {
        foreach (var (arch, generation) in NpuGenerations)
        {
            if (name.Contains($"(arch {arch})", StringComparison.Ordinal))
            {
                return name.Replace($"(arch {arch})", $"· {generation} (arch {arch})");
            }
        }

        return name;
    }

    private static bool SameDevice(string a, string b)
    {
        static string Normalise(string name) => name
            .Replace("(R)", "").Replace("(TM)", "").Replace("  ", " ")
            .ToUpperInvariant().Trim();
        var (x, y) = (Normalise(a), Normalise(b));
        return x.Contains(y, StringComparison.Ordinal)
            || y.Contains(x, StringComparison.Ordinal)
            || (x.Contains("AI BOOST", StringComparison.Ordinal)
                && y.Contains("AI BOOST", StringComparison.Ordinal));
    }

    private static double? Delta(double? from, double? to) =>
        from is null || to is null ? null : Math.Max(0, Math.Round(to.Value - from.Value, 1));

    private static double Median(List<double> sorted) =>
        sorted.Count % 2 == 1
            ? sorted[sorted.Count / 2]
            : (sorted[sorted.Count / 2 - 1] + sorted[sorted.Count / 2]) / 2;

    private static string Clock(double? seconds) => seconds is null ? "-" : Words.Clock(seconds.Value);

    private static string Format(double? value) => value is null
        ? "–"
        : value.Value.ToString(Math.Abs(value.Value) >= 100 ? "0" : "0.0",
            CultureInfo.InvariantCulture);

    private static void Row(StringBuilder html, string label, string value) =>
        html.Append(CultureInfo.InvariantCulture,
            $"<tr><th>{WebUtility.HtmlEncode(label)}</th>"
            + $"<td>{WebUtility.HtmlEncode(value)}</td></tr>");

    private static void Cell(StringBuilder html, string? value, string? cls = null) =>
        html.Append(CultureInfo.InvariantCulture,
            $"<td{(cls is null ? "" : $" class=\"{cls}\"")}>{WebUtility.HtmlEncode(value ?? "–")}</td>");
}
