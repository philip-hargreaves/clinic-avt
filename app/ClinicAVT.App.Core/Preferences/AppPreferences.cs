using System.Globalization;
using System.Text.Json;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Preferences;

/// <summary>
/// App preferences as one small json document. A missing document means defaults. An unreadable one
/// means defaults and a log line. Values the shell or engine would reject are dropped on load.
/// </summary>
public sealed class AppPreferences(IPreferencesStore store, ILogger? logger = null)
{
    private sealed record PreferencesFile
    {
        public int SchemaVersion { get; init; } = CurrentSchema;

        public bool SeedDataEnabled { get; init; }

        public bool NpuTranscription { get; init; }

        public bool CollectPerformanceData { get; init; }

        public bool KeepConsultations { get; init; } = true;

        public bool ShowPerformanceMetrics { get; init; } = true;

        public bool IncludeResearchGuidance { get; init; }

        public string? MicId { get; init; }

        public string? Theme { get; init; }

        public string? NoteStyle { get; init; }

        public string? NoteDetail { get; init; }

        public string? NoteTier { get; init; }

        public LastBackup? LastBackup { get; init; }
    }

    public const int CurrentSchema = 1;

    // The file's names for each theme, in AppTheme order
    private static readonly IReadOnlyList<string> ThemeNames = ["system", "light", "dark"];

    /// <summary>The note model tiers the engine's store can resolve, in ladder order.</summary>
    public static readonly IReadOnlyList<string> NoteTiers = ["constrained", "default", "accuracy"];

    /// <summary>A note model never chosen. The engine picks one for the machine.</summary>
    public const string AutoNoteTier = "auto";

    /// <summary>
    /// Raised after every save, so a page can follow a preference it does not own.
    /// </summary>
    public event Action? Saved;

    public bool SeedDataEnabled { get; set; }

    public bool NpuTranscription { get; set; }

    public bool CollectPerformanceData { get; set; }

    /// <summary>
    /// On by default, because this build is used for demonstrations and the Sessions and
    /// Appraisal pages need stored consultations. A change applies to consultations recorded
    /// after it.
    /// </summary>
    public bool KeepConsultations { get; set; } = true;

    /// <summary>On by default, so evaluators see the status-bar timings from the first run.</summary>
    public bool ShowPerformanceMetrics { get; set; } = true;

    /// <summary>
    /// Searches corpora marked research, such as the local NICE demo. Only a debug build shows
    /// or sets it. It reaches the engine as --include-research at launch.
    /// </summary>
    public bool IncludeResearchGuidance { get; set; }

    /// <summary>The chosen microphone's endpoint id. Empty means the default.</summary>
    public string MicId { get; set; } = "";

    public AppTheme Theme { get; set; } = AppTheme.System;

    public string NoteStyle { get; set; } = NoteOptions.DefaultStyle.Value;

    public string NoteDetail { get; set; } = NoteOptions.DefaultDetail.Value;

    /// <summary>
    /// Which note model the engine loads, as a role such as "default", "accuracy" or
    /// "constrained". The engine's store resolves it to a model. "auto" until one is chosen.
    /// </summary>
    public string NoteTier { get; set; } = AutoNoteTier;

    /// <summary>The last checked backup, null before the first.</summary>
    public LastBackup? LastBackup { get; set; }

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
        preferences.SeedDataEnabled = stored.SeedDataEnabled;
        preferences.NpuTranscription = stored.NpuTranscription;
        preferences.CollectPerformanceData = stored.CollectPerformanceData;
        preferences.KeepConsultations = stored.KeepConsultations;
        preferences.ShowPerformanceMetrics = stored.ShowPerformanceMetrics;
        preferences.IncludeResearchGuidance = stored.IncludeResearchGuidance;
        preferences.MicId = stored.MicId ?? "";
        preferences.Theme = (AppTheme)Math.Max(0, ThemeNames.ToList().IndexOf(stored.Theme ?? ""));
        preferences.NoteStyle = NoteOptions.Style(stored.NoteStyle).Value;
        preferences.NoteDetail = NoteOptions.Detail(stored.NoteDetail).Value;
        preferences.NoteTier = Known(stored.NoteTier, [.. NoteTiers, AutoNoteTier], AutoNoteTier);
        preferences.LastBackup = stored.LastBackup is { From: not null, To: not null } last
            && DateTimeOffset.TryParse(last.CreatedAt, CultureInfo.InvariantCulture, out _)
                ? last
                : null;
        return preferences;
    }

    public void Update(Action<AppPreferences> change)
    {
        change(this);
        Save();
    }

    public void Save()
    {
        try
        {
            store.Write(JsonSerializer.Serialize(new PreferencesFile
            {
                SeedDataEnabled = SeedDataEnabled,
                NpuTranscription = NpuTranscription,
                CollectPerformanceData = CollectPerformanceData,
                KeepConsultations = KeepConsultations,
                ShowPerformanceMetrics = ShowPerformanceMetrics,
                IncludeResearchGuidance = IncludeResearchGuidance,
                MicId = MicId,
                Theme = ThemeNames[(int)Theme],
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
