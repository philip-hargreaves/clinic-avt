namespace ClinicAVT.App.Platform.Tests;

public sealed class FileStoresTest : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());

    public void Dispose()
    {
        if (Directory.Exists(_dir))
        {
            Directory.Delete(_dir, recursive: true);
        }
    }

    [Fact]
    public void PreferencesReadNothingUntilWrittenThenCreateTheirFolderAndReadBack()
    {
        var store = new FilePreferencesStore(Path.Combine(_dir, "ClinicAVT", "preferences.json"));
        Assert.Null(store.Read());

        store.Write("""{"KeepConsultations":true}""");

        Assert.Equal("""{"KeepConsultations":true}""", store.Read());
    }

    [Fact]
    public async Task TheMetricsLogAppendsALineAtATimeAndTextFilesWriteWhole()
    {
        var log = new FileMetricsLog(Path.Combine(_dir, "metrics.jsonl"));
        Assert.Null(log.ReadAll());

        log.Append("{\"schema\":2}");
        log.Append("{\"schema\":2,\"outcome\":\"refused\"}");

        Assert.Equal(["{\"schema\":2}", "{\"schema\":2,\"outcome\":\"refused\"}"], log.ReadAll());

        var report = Path.Combine(_dir, "report.html");
        await new TextFiles().WriteAsync(report, "<p>é</p>");
        Assert.Equal("<p>é</p>", await File.ReadAllTextAsync(report));
    }
}
