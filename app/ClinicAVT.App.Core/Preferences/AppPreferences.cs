using System.Globalization;
using System.Text.Json;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Preferences;

/// <summary>
/// One small json document of app preferences. Absent means defaults, unreadable means
/// defaults and a log line. Values the shell cannot render or the engine would refuse never
/// leave the load boundary.
/// </summary>
public sealed class AppPreferences(IPreferencesStore store, ILogger? logger = null)
{
    /// <summary>The json as written, with named fields and a schema version.</summary>
    private sealed record PreferencesFile
    {
        public int SchemaVersion { get; init; } = CurrentSchema;

        public bool DemoTrayEnabled { get; init; }

        public bool DemoMode { get; init; }

        public string? DemoTrack { get; init; }

        public bool SeedDataEnabled { get; init; }

        public bool NpuTranscription { get; init; }

        public bool CollectPerformanceData { get; init; }

        public bool KeepConsultations { get; init; }

        public bool ShowPerformanceMetrics { get; init; }

        public bool IncludeResearchGuidance { get; init; }

        public string? MicId { get; init; }

        public string? Theme { get; init; }

        public string? NoteStyle { get; init; }

        public string? NoteDetail { get; init; }

        public string? NoteTier { get; init; }

        public LastBackup? LastBackup { get; init; }
    }

    public const int CurrentSchema = 1;

    /// <summary>The themes the shell can render, the default first.</summary>
    public static readonly IReadOnlyList<string> Themes = ["system", "light", "dark"];

    /// <summary>The note model tiers the engine's store can resolve, in ladder order.</summary>
    public static readonly IReadOnlyList<string> NoteTiers = ["constrained", "default", "accuracy"];

    /// <summary>A note model never chosen: the engine picks one for the machine.</summary>
    public const string AutoNoteTier = "auto";

    public AppPreferences(string path, ILogger? logger = null)
        : this(new FilePreferencesStore(path), logger)
    {
    }

    /// <summary>
    /// Raised after every save, so a page can follow a preference it does not own.
    /// </summary>
    public event Action? Saved;

    public bool DemoTrayEnabled { get; set; }

    /// <summary>Record plays a saved run back. A developer control.</summary>
    public bool DemoMode { get; set; }

    /// <summary>The track whose saved run demo mode plays. Empty means the first.</summary>
    public string DemoTrack { get; set; } = "";

    public bool SeedDataEnabled { get; set; }

    public bool NpuTranscription { get; set; }

    public bool CollectPerformanceData { get; set; }

    /// <summary>
    /// Off by default, so the app saves nothing beyond the consultation unless the clinician
    /// opts in. A change applies to consultations recorded after it.
    /// </summary>
    public bool KeepConsultations { get; set; }

    /// <summary>Off by default because the status-bar chips are for testing.</summary>
    public bool ShowPerformanceMetrics { get; set; }

    /// <summary>
    /// Searches corpora marked research, such as the local NICE demo. Only a debug build shows
    /// or sets it. It reaches the engine as --include-research at launch.
    /// </summary>
    public bool IncludeResearchGuidance { get; set; }

    /// <summary>The chosen microphone's endpoint id. Empty means the default.</summary>
    public string MicId { get; set; } = "";

    /// <summary>"system" follows the OS, while "light" and "dark" override it.</summary>
    public string Theme { get; set; } = Themes[0];

    public string NoteStyle { get; set; } = NoteOptions.DefaultStyle.Value;

    public string NoteDetail { get; set; } = NoteOptions.DefaultDetail.Value;

    /// <summary>
    /// Which note model the engine loads, as a role such as "default", "accuracy" or
    /// "constrained". The engine's store resolves it to a model. "auto" until one is chosen.
    /// </summary>
    public string NoteTier { get; set; } = AutoNoteTier;

    /// <summary>The last checked backup, null before the first.</summary>
    public LastBackup? LastBackup { get; set; }

    public static AppPreferences Load(string path, ILogger? logger = null) =>
        Load(new FilePreferencesStore(path), logger);

    public static AppPreferences Load(IPreferencesStore store, ILogger? logger = null)
    {
        var preferences = new AppPreferences(store, logger);
        PreferencesFile? stored;
        try
        {
            var json = store.Read();
            if (json is null)
            {
                return preferences;
            }

            stored = JsonSerializer.Deserialize<PreferencesFile>(json);
        }
        catch (Exception e)
        {
            // A corrupt document means defaults. The next save replaces it
            logger?.PreferencesUnreadable(e.Message);
            return preferences;
        }

        if (stored is null)
        {
            return preferences;
        }

        // A newer document is read for what this build knows. A save rewrites it at this schema
        preferences.DemoTrayEnabled = stored.DemoTrayEnabled;
        preferences.DemoMode = stored.DemoMode;
        preferences.DemoTrack = stored.DemoTrack ?? "";
        preferences.SeedDataEnabled = stored.SeedDataEnabled;
        preferences.NpuTranscription = stored.NpuTranscription;
        preferences.CollectPerformanceData = stored.CollectPerformanceData;
        preferences.KeepConsultations = stored.KeepConsultations;
        preferences.ShowPerformanceMetrics = stored.ShowPerformanceMetrics;
        preferences.IncludeResearchGuidance = stored.IncludeResearchGuidance;
        preferences.MicId = stored.MicId ?? "";
        preferences.Theme = Known(stored.Theme, Themes, Themes[0]);
        preferences.NoteStyle = NoteOptions.Style(stored.NoteStyle).Value;
        preferences.NoteDetail = NoteOptions.Detail(stored.NoteDetail).Value;
        preferences.NoteTier = Known(stored.NoteTier, [.. NoteTiers, AutoNoteTier], AutoNoteTier);
        preferences.LastBackup = stored.LastBackup is { From: not null, To: not null } last
            && DateTimeOffset.TryParse(last.CreatedAt, CultureInfo.InvariantCulture, out _)
                ? last
                : null;
        return preferences;
    }

    public void Save()
    {
        try
        {
            store.Write(JsonSerializer.Serialize(new PreferencesFile
            {
                DemoTrayEnabled = DemoTrayEnabled,
                DemoMode = DemoMode,
                DemoTrack = DemoTrack,
                SeedDataEnabled = SeedDataEnabled,
                NpuTranscription = NpuTranscription,
                CollectPerformanceData = CollectPerformanceData,
                KeepConsultations = KeepConsultations,
                ShowPerformanceMetrics = ShowPerformanceMetrics,
                IncludeResearchGuidance = IncludeResearchGuidance,
                MicId = MicId,
                Theme = Theme,
                NoteStyle = NoteStyle,
                NoteDetail = NoteDetail,
                NoteTier = NoteTier,
                LastBackup = LastBackup,
            }));
        }
        catch (Exception e)
        {
            logger?.PreferencesNotSaved(e.Message);
        }

        Saved?.Invoke();
    }

    private static string Known(string? value, IReadOnlyList<string> known, string fallback) =>
        value is not null && known.Contains(value) ? value : fallback;
}
