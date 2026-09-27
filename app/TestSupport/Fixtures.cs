using System.Text.Json;

namespace ClinicAVT.TestSupport;

/// <summary>The shared wire fixtures under schema/fixtures, found from the test output.</summary>
internal static class Fixtures
{
    public static JsonElement Load(string name)
    {
        var dir = AppContext.BaseDirectory;
        while (dir is not null && !Directory.Exists(Path.Combine(dir, "schema", "fixtures")))
        {
            dir = Path.GetDirectoryName(dir);
        }

        Assert.NotNull(dir);
        return JsonDocument.Parse(
            File.ReadAllText(Path.Combine(dir, "schema", "fixtures", name))).RootElement.Clone();
    }
}
