using System.Text.Json;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Features.Examples;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Platform;

/// <summary>The example cases and recordings in demo\ beside the app.</summary>
public sealed class ExampleLibrary(string baseDirectory, ILogger<ExampleLibrary> logger) : IExampleLibrary
{
    /// <summary>
    /// Loads demo/cases.txt, probing upward like the tracks manifest. Blank lines separate the
    /// cases. Each has a title line, an optional dashed underline, then the text. A missing or
    /// empty file means no cases, and an unreadable one is logged.
    /// </summary>
    public IReadOnlyList<ExampleCase> LoadCases()
    {
        if (Find("cases.txt") is not { } file)
        {
            return [];
        }

        try
        {
            return ParseCases(File.ReadAllText(file));
        }
        catch (Exception e)
        {
            logger.StepFailed("example cases", e.Message);
            return [];
        }
    }

    /// <summary>
    /// Loads demo/tracks.json, probing upward so packaged and dev layouts
    /// both resolve. A missing manifest or wav is skipped, and an unreadable manifest is logged.
    /// </summary>
    public IReadOnlyList<ExampleRecording> LoadRecordings() =>
        Find("tracks.json") is { } manifest ? ParseRecordings(manifest, logger) : [];

    public static IReadOnlyList<ExampleCase> ParseCases(string text)
    {
        var cases = new List<ExampleCase>();
        foreach (var block in text.Replace("\r\n", "\n").Split("\n\n", StringSplitOptions.RemoveEmptyEntries))
        {
            var lines = block.Split('\n', StringSplitOptions.RemoveEmptyEntries)
                .Select(l => l.Trim())
                .Where(l => l.Any(c => c != '-'))
                .ToList();
            if (lines.Count >= 2)
            {
                cases.Add(new ExampleCase(lines[0], string.Join(" ", lines.Skip(1))));
            }
        }

        return cases;
    }

    public static IReadOnlyList<ExampleRecording> ParseRecordings(string manifestPath, ILogger logger)
    {
        try
        {
            var root = Path.GetDirectoryName(manifestPath)!;
            using var json = JsonDocument.Parse(File.ReadAllText(manifestPath));
            var tracks = new List<ExampleRecording>();
            foreach (var entry in json.RootElement.GetProperty("tracks").EnumerateArray())
            {
                var name = entry.GetProperty("name").GetString();
                var file = entry.GetProperty("file").GetString();
                if (string.IsNullOrEmpty(name) || string.IsNullOrEmpty(file))
                {
                    continue;
                }

                var path = Path.Combine(root, file);
                if (File.Exists(path))
                {
                    tracks.Add(new ExampleRecording(name, path, DurationSeconds(path, logger)));
                }
            }

            return tracks;
        }
        catch (Exception e)
        {
            logger.StepFailed("example recordings", e.Message);
            return [];
        }
    }

    /// <summary>Duration of a 16 kHz mono wav, from its header alone. 0 when unreadable.</summary>
    public static double DurationSeconds(string path, ILogger logger)
    {
        try
        {
            using var stream = File.OpenRead(path);
            using var reader = new BinaryReader(stream);
            if (new string(reader.ReadChars(4)) != "RIFF")
            {
                return 0;
            }

            reader.ReadUInt32();
            if (new string(reader.ReadChars(4)) != "WAVE")
            {
                return 0;
            }

            ushort bits = 16;
            while (stream.Position + 8 <= stream.Length)
            {
                var tag = new string(reader.ReadChars(4));
                var size = reader.ReadUInt32();
                if (tag == "fmt ")
                {
                    var chunk = reader.ReadBytes((int)size + (int)(size & 1));
                    bits = BitConverter.ToUInt16(chunk, 14);
                }
                else if (tag == "data")
                {
                    return size / (16000.0 * (bits / 8.0));
                }
                else
                {
                    stream.Seek(size + (size & 1), SeekOrigin.Current);
                }
            }

            return 0;
        }
        catch (Exception e)
        {
            logger.StepFailed($"example recording {Path.GetFileName(path)}", e.Message);
            return 0;
        }
    }

    // Packaged debug runs sit one level deeper under AppX, so the search climbs several folders
    private string? Find(string name)
    {
        var dir = baseDirectory;
        for (var i = 0; i < 10 && dir is not null; i++, dir = Directory.GetParent(dir)?.FullName)
        {
            var file = Path.Combine(dir, "demo", name);
            if (File.Exists(file))
            {
                return file;
            }
        }

        return null;
    }
}
