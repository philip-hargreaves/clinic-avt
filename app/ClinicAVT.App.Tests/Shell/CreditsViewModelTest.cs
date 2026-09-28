using ClinicAVT.App.Core.Shell;

namespace ClinicAVT.App.Tests.Shell;

public class CreditsViewModelTest : IDisposable
{
    private readonly string _directory =
        Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());

    public CreditsViewModelTest() => Directory.CreateDirectory(_directory);

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
        Assert.Empty(new CreditsViewModel(_directory).Marks);
        Write("credits.json", "{ not json");
        Assert.Empty(new CreditsViewModel(_directory).Marks);

        Write("a.png");
        Write("a-dark.png");
        Write("b.svg");
        Write("credits.json", """
            { "marks": [
                { "name": "A", "light": "a.png", "dark": "a-dark.png", "height": 18 },
                { "name": "B", "light": "b.svg", "space": 4 }
            ] }
            """);

        var credits = new CreditsViewModel(_directory);

        Assert.Equal(2, credits.Marks.Count);
        Assert.Equal("A", credits.Marks[0].Name);
        Assert.EndsWith("a-dark.png", credits.Marks[0].DarkPath);
        Assert.Equal(18, credits.Marks[0].Height);
        Assert.Equal(credits.Marks[1].LightPath, credits.Marks[1].DarkPath);
        Assert.Equal(16, credits.Marks[1].Height);
        Assert.Equal(0, credits.Marks[0].Space);
        Assert.Equal(4, credits.Marks[1].Space);

        // A missing image skips its mark only
        File.Delete(Path.Combine(_directory, "a.png"));
        var mark = Assert.Single(new CreditsViewModel(_directory).Marks);
        Assert.Equal("B", mark.Name);
    }
}
