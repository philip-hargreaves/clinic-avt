namespace ClinicAVT.App.Platform.Tests;

public class CreditsFileTest : IDisposable
{
    private readonly string _directory =
        Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());

    public CreditsFileTest() => Directory.CreateDirectory(_directory);

    public void Dispose()
    {
        Directory.Delete(_directory, recursive: true);
        GC.SuppressFinalize(this);
    }

    private void Write(string name, string content = "x") =>
        File.WriteAllText(Path.Combine(_directory, name), content);

    [Fact]
    public void MarksLoadInManifestOrderAndWhatIsMissingIsSkipped()
    {
        // No manifest and a broken one both give an empty row
        Assert.Empty(new CreditsFile(_directory).LoadMarks());
        Write("credits.json", "{ not json");
        Assert.Empty(new CreditsFile(_directory).LoadMarks());

        Write("a.png");
        Write("a-dark.png");
        Write("b.svg");
        Write("credits.json", """
            { "marks": [
                { "name": "A", "light": "a.png", "dark": "a-dark.png", "height": 18 },
                { "name": "B", "light": "b.svg", "space": 4 }
            ] }
            """);

        var marks = new CreditsFile(_directory).LoadMarks();

        Assert.Equal(2, marks.Count);
        Assert.Equal("A", marks[0].Name);
        Assert.EndsWith("a-dark.png", marks[0].DarkPath);
        Assert.Equal(18, marks[0].Height);
        Assert.Equal(marks[1].LightPath, marks[1].DarkPath);
        Assert.Equal(16, marks[1].Height);
        Assert.Equal(0, marks[0].Space);
        Assert.Equal(4, marks[1].Space);

        // A missing image skips its mark only
        File.Delete(Path.Combine(_directory, "a.png"));
        var mark = Assert.Single(new CreditsFile(_directory).LoadMarks());
        Assert.Equal("B", mark.Name);
    }
}
