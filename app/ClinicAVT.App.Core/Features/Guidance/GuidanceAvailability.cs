using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Guidance;

/// <summary>Whether guidance can be searched, as guidance/corpora reports it.</summary>
public sealed partial class GuidanceAvailability : ObservableObject
{
    [ObservableProperty]
    public partial GuidanceReadiness Readiness { get; private set; } = GuidanceReadiness.Loading;

    /// <summary>The loader's reason when unavailable, for the log. Never shown.</summary>
    public string ReadinessDetail { get; private set; } = "";

    /// <summary>Corpora the engine refused, as "id: reason", for the log.</summary>
    public IReadOnlyList<string> RefusedCorpora { get; private set; } = [];

    /// <summary>
    /// Takes the embedder's state from guidance/corpora. Searching needs only that, since added
    /// documents are searched whether or not any corpus is installed.
    /// </summary>
    public void ApplyCorpora(CorporaStatus corpora)
    {
        ReadinessDetail = corpora.Detail ?? "";
        var refused = new List<string>();
        foreach (var corpus in corpora.Corpora)
        {
            var reason = corpus.Unavailable ?? "";
            if (reason.Length > 0)
            {
                refused.Add($"{corpus.Id}: {reason}");
            }
        }

        RefusedCorpora = refused;
        Readiness = corpora.State switch
        {
            CorporaState.Loading => GuidanceReadiness.Loading,
            CorporaState.Ready => GuidanceReadiness.Ready,
            _ => GuidanceReadiness.Unavailable,
        };
    }

    /// <summary>The poll itself failed, so there is nothing to wait for.</summary>
    public void CorporaUnavailable()
    {
        ReadinessDetail = "";
        RefusedCorpora = [];
        Readiness = GuidanceReadiness.Unavailable;
    }
}
