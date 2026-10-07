using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Settings;

public sealed partial class GuidanceCorporaViewModel : ObservableObject
{
    private readonly AppPreferences _preferences;
    private readonly IGuidanceApi _engine;
    private readonly IStatusLine _status;
    private readonly bool _initialising;

    public GuidanceCorporaViewModel(
        AppPreferences preferences, IGuidanceApi engine, IEngineEvents events, IStatusLine status)
    {
        _preferences = preferences;
        _engine = engine;
        _status = status;
        // Loading saved values, so setters skip their change handling
        _initialising = true;
        IncludeResearchGuidance = preferences.IncludeResearchGuidance;
        _initialising = false;
        events.OnConnected(() => _ = LoadGuidanceCorporaAsync());
        events.Subscribe<GuidanceModelChanged>(changed => _ = LoadGuidanceCorporaAsync());
    }

    /// <summary>The corpora the engine has, refused ones included.</summary>
    public ObservableCollection<CorpusRow> GuidanceCorpora { get; } = [];

    /// <summary>Why nothing is listed, empty when corpora are shown.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(GuidanceCaptionVisible), nameof(GuidanceInstalledVisible))]
    public partial string GuidanceCaption { get; set; } = "";

    public bool GuidanceCaptionVisible => GuidanceCaption.Length > 0;

    /// <summary>Hidden when nothing is installed, so no empty card shows.</summary>
    public bool GuidanceInstalledVisible => GuidanceCorpora.Count > 0 || GuidanceCaption.Length > 0;

    /// <summary>
    /// Shows the switch for the local research corpus, the NICE demo, in a debug build only.
    /// </summary>
    public bool ResearchToggleVisible { get; } = BuildFlags.Debug;

    [ObservableProperty]
    public partial bool IncludeResearchGuidance { get; set; }

    partial void OnIncludeResearchGuidanceChanged(bool value)
    {
        if (_initialising)
        {
            return;
        }

        _preferences.Update(p => p.IncludeResearchGuidance = value);
        _ = ApplyResearchAsync(value);
    }

    // The engine reloads its corpora live and announces the change, so the
    // list follows without a restart
    private async Task ApplyResearchAsync(bool include)
    {
        if (!_engine.Connected)
        {
            return;
        }

        await EngineCall.LogAsync(_status, "guidance/research",
            () => _engine.SetResearchGuidanceAsync(include)).ConfigureAwait(true);
    }

    private async Task LoadGuidanceCorporaAsync()
    {
        if (!_engine.Connected)
        {
            return;
        }

        if (!await EngineCall.LogAsync(_status, "guidance/corpora",
                async () => ApplyGuidanceCorpora(await _engine.GuidanceCorporaAsync().ConfigureAwait(true)))
            .ConfigureAwait(true))
        {
            GuidanceCorpora.Clear();
            GuidanceCaption = "Unavailable";
        }
    }

    private void ApplyGuidanceCorpora(CorporaStatus reply)
    {
        GuidanceCorpora.Clear();
        foreach (var corpus in reply.Corpora)
        {
            GuidanceCorpora.Add(RowFrom(corpus));
        }

        var detail = reply.Detail ?? "";
        GuidanceCaption = reply.State switch
        {
            CorporaState.Loading => "Loading",
            CorporaState.Unavailable => detail.Length > 0 ? $"Unavailable: {detail}" : "Unavailable",
            _ => "",
        };
        OnPropertyChanged(nameof(GuidanceInstalledVisible));
    }

    // A refused corpus keeps its place, so unused guidance stays visible
    private static CorpusRow RowFrom(CorpusInfo corpus)
    {
        var refused = corpus.Unavailable ?? "";
        if (refused.Length > 0)
        {
            return new CorpusRow(corpus.Id, $"Not used: {refused}", "", true);
        }

        // The licence is not shown because the attribution line is what it asks for
        var parts = new List<string>();
        if (corpus.Chunks > 0)
        {
            parts.Add($"{corpus.Chunks:N0} passages");
        }

        var built = Words.ShortDate(corpus.BuiltAt ?? "");
        if (built.Length > 0)
        {
            parts.Add(built);
        }

        return new CorpusRow(corpus.Name, string.Join(" · ", parts), corpus.Attribution ?? "", false);
    }
}
