using System.Text.Json;

namespace ClinicAVT.App.Core.Shell;

/// <summary>
/// The credits row, driven by Assets/logos/credits.json so a showcase can add
/// or remove marks by editing the deployed folder without a code change. A
/// missing file skips its mark. A missing or broken manifest yields an empty row.
/// </summary>
public sealed class CreditsViewModel
{
    public CreditsViewModel(string? logosDirectory = null)
    {
        var directory = logosDirectory
            ?? Path.Combine(AppContext.BaseDirectory, "Assets", "logos");
        Marks = Load(directory);
    }

    public IReadOnlyList<CreditMark> Marks { get; }

    private static List<CreditMark> Load(string directory)
    {
        var manifest = Path.Combine(directory, "credits.json");
        if (!File.Exists(manifest))
        {
            return [];
        }

        try
        {
            using var parsed = JsonDocument.Parse(File.ReadAllText(manifest));
            var marks = new List<CreditMark>();
            foreach (var entry in parsed.RootElement.GetProperty("marks").EnumerateArray())
            {
                var light = entry.GetProperty("light").GetString() ?? "";
                var lightPath = Path.Combine(directory, light);
                if (!File.Exists(lightPath))
                {
                    Console.Error.WriteLine($"credits: {light} missing, mark skipped");
                    continue;
                }

                var dark = entry.TryGetProperty("dark", out var d) ? d.GetString() : null;
                var darkPath = dark is null ? lightPath : Path.Combine(directory, dark);
                marks.Add(new CreditMark(
                    entry.GetProperty("name").GetString() ?? "",
                    lightPath,
                    File.Exists(darkPath) ? darkPath : lightPath,
                    entry.TryGetProperty("height", out var h) ? h.GetDouble() : 16,
                    entry.TryGetProperty("space", out var s) ? s.GetDouble() : 0));
            }

            return marks;
        }
        catch (JsonException e)
        {
            Console.Error.WriteLine($"credits: manifest unreadable ({e.Message})");
            return [];
        }
    }
}
