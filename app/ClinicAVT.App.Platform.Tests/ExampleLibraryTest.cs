using Microsoft.Extensions.Logging.Abstractions;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Platform.Tests;

public class ExampleLibraryTest
{
    [Fact]
    public void CasesParseAsTitledBlocksAndTheShippedFileLoads()
    {
        var cases = ExampleLibrary.ParseCases("""
            Case 1, rheumatoid arthritis
            ----------------------------
            65-year-old man, three-month history of stiffness.
            Plan: refer urgently.

            Case 2, gout
            ------------
            38-year-old man.
            """);

        Assert.Equal(2, cases.Count);
        Assert.Equal("Case 1, rheumatoid arthritis", cases[0].Title);
        Assert.Equal("65-year-old man, three-month history of stiffness. Plan: refer urgently.", cases[0].Text);
        Assert.Equal("38-year-old man.", cases[1].Text);
        Assert.Empty(ExampleLibrary.ParseCases(""));

        var shipped = new ExampleLibrary(AppContext.BaseDirectory, NullLogger<ExampleLibrary>.Instance).LoadCases();
        Assert.Equal(4, shipped.Count);
        Assert.All(shipped, c => Assert.StartsWith("Case ", c.Title));
        Assert.All(shipped, c => Assert.True(c.Text.Split(' ').Length > 60, c.Title));
    }

    [Fact]
    public void ManifestListsOnlyTracksWhoseWavsExistABrokenOneYieldsNoneAndDurationComesFromTheHeader()
    {
        var root = Directory.CreateTempSubdirectory("clinicavt-demo-test");
        var manifest = Path.Combine(root.FullName, "tracks.json");
        var log = new ListLogger();
        try
        {
            File.WriteAllText(manifest, "not json");
            Assert.Empty(ExampleLibrary.ParseRecordings(manifest, log));
            Assert.Contains(log.Lines, line => line.Contains("example recordings failed"));

            var wav = Path.Combine(root.FullName, "elbow.wav");
            File.WriteAllBytes(wav, new byte[44]);
            File.WriteAllText(manifest, """
                {"tracks":[
                  {"name":"Elbow swelling","file":"elbow.wav"},
                  {"name":"Missing","file":"gone.wav"}
                ]}
                """);

            var track = Assert.Single(ExampleLibrary.ParseRecordings(manifest, NullLogger.Instance));
            Assert.Equal("Elbow swelling", track.Name);
            Assert.Equal(wav, track.Path);
        }
        finally
        {
            root.Delete(recursive: true);
        }

        var real = SilenceWav.Write(seconds: 3);
        try
        {
            Assert.Equal(3.0, ExampleLibrary.DurationSeconds(real, NullLogger.Instance), 3);
        }
        finally
        {
            File.Delete(real);
        }

        Assert.Equal(0, ExampleLibrary.DurationSeconds("C:/does/not/exist.wav", log));
        Assert.Contains(log.Lines, line => line.Contains("example recording exist.wav failed"));
    }
}
