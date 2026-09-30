using System.Text.Json;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Preferences;

public class AppPreferencesTest
{
    [Fact]
    public void EveryValueRoundTripsThroughTheStore()
    {
        var store = new MemoryPreferencesStore();
        var preferences = new AppPreferences(store)
        {
            SeedDataEnabled = true,
            NpuTranscription = true,
            CollectPerformanceData = true,
            KeepConsultations = true,
            ShowPerformanceMetrics = false,  // on by default, so off is what round-trips
            IncludeResearchGuidance = true,
            MicId = "{mic-7}",
            Theme = AppTheme.Dark,
            NoteStyle = "soap",
            NoteDetail = "detailed",
            NoteTier = "accuracy",
        };

        preferences.Save();
        var loaded = AppPreferences.Load(store);

        var version = JsonDocument.Parse(store.Json!).RootElement.GetProperty("SchemaVersion").GetInt32();
        Assert.Equal(AppPreferences.CurrentSchema, version);
        Assert.True(loaded.SeedDataEnabled);
        Assert.True(loaded.NpuTranscription);
        Assert.True(loaded.CollectPerformanceData);
        Assert.True(loaded.KeepConsultations);
        Assert.False(loaded.ShowPerformanceMetrics);
        Assert.True(loaded.IncludeResearchGuidance);
        Assert.Equal("{mic-7}", loaded.MicId);
        Assert.Equal(AppTheme.Dark, loaded.Theme);
        Assert.Contains("\"Theme\":\"dark\"", store.Json);
        Assert.Equal("soap", loaded.NoteStyle);
        Assert.Equal("detailed", loaded.NoteDetail);
        Assert.Equal("accuracy", loaded.NoteTier);
    }

    [Fact]
    public void NothingStoredOrUnreadableMeansDefaults()
    {
        var empty = new MemoryPreferencesStore();
        var loaded = AppPreferences.Load(empty);
        Assert.False(loaded.SeedDataEnabled);
        Assert.Null(empty.Json);  // no write on load

        var corrupt = new MemoryPreferencesStore { Json = "{ this is not json" };
        var log = new ListLogger();
        loaded = AppPreferences.Load(corrupt, log);
        Assert.True(loaded.KeepConsultations);
        Assert.Equal(AppTheme.System, loaded.Theme);
        Assert.Contains(log.Lines, line => line.Contains("preferences unreadable"));
    }

    // A note model never chosen is left to the engine, and the old middle length reads as concise
    [Fact]
    public void ANewerDocumentIsReadForWhatThisBuildKnowsAndOldOrUnknownValuesFallBack()
    {
        var newer = new MemoryPreferencesStore
        {
            Json = """{"SchemaVersion":99,"KeepConsultations":true,"FutureSetting":"x"}""",
        };
        Assert.True(AppPreferences.Load(newer).KeepConsultations);

        var odd = new MemoryPreferencesStore
        {
            Json = """{"Theme":"solarized","NoteStyle":"haiku","NoteDetail":"verbose","NoteTier":"premium"}""",
        };
        var loaded = AppPreferences.Load(odd);
        Assert.Equal(AppTheme.System, loaded.Theme);
        Assert.Equal("prose", loaded.NoteStyle);
        Assert.Equal("concise", loaded.NoteDetail);
        Assert.Equal("auto", loaded.NoteTier);

        Assert.Equal("auto", AppPreferences.Load(new MemoryPreferencesStore()).NoteTier);
        var older = new MemoryPreferencesStore { Json = """{"KeepConsultations":true,"NoteDetail":"standard"}""" };
        Assert.Equal("auto", AppPreferences.Load(older).NoteTier);
        Assert.Equal("concise", AppPreferences.Load(older).NoteDetail);
        Assert.Equal(["concise", "detailed"], NoteOptions.Details.Select(d => d.Value));
        var chosen = new MemoryPreferencesStore { Json = """{"NoteTier":"accuracy"}""" };
        Assert.Equal("accuracy", AppPreferences.Load(chosen).NoteTier);
    }
}
