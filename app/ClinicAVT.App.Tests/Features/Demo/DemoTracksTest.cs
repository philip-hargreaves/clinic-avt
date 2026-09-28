using ClinicAVT.App.Core.Features.Demo;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Features.Demo;

public class DemoTracksTest
{
    [Fact]
    public void ManifestListsOnlyTracksWhoseWavsExistABrokenOneYieldsNoneAndDurationComesFromTheHeader()
    {
        var root = Directory.CreateTempSubdirectory("clinicavt-demo-test");
        var manifest = Path.Combine(root.FullName, "tracks.json");
        var log = new ListLogger();
        try
        {
            File.WriteAllText(manifest, "not json");
            Assert.Empty(DemoTracks.Parse(manifest, log));
            Assert.Contains(log.Lines, line => line.Contains("example recordings failed"));

            var wav = Path.Combine(root.FullName, "elbow.wav");
            File.WriteAllBytes(wav, new byte[44]);
            File.WriteAllText(manifest, """
                {"tracks":[
                  {"name":"Elbow swelling","file":"elbow.wav"},
                  {"name":"Missing","file":"gone.wav"}
                ]}
                """);

            var track = Assert.Single(DemoTracks.Parse(manifest));
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
            Assert.Equal(3.0, DemoTracks.DurationSeconds(real), 3);
        }
        finally
        {
            File.Delete(real);
        }

        Assert.Equal(0, DemoTracks.DurationSeconds("C:/does/not/exist.wav", log));
        Assert.Contains(log.Lines, line => line.Contains("example recording exist.wav failed"));
    }
}
