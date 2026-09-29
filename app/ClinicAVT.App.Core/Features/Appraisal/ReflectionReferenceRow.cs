using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Appraisal;

/// <summary>
/// One guideline or document the consultation's guidance drew on, ticked when the clinician
/// referred to it. Grouped as the review's cards are, so one document is one row.
/// </summary>
public sealed partial class ReflectionReferenceRow : ObservableObject
{
    public ReflectionReferenceRow(ReflectionReference stored, bool ticked)
    {
        Stored = stored;
        Ticked = ticked;
    }

    public static ReflectionReferenceRow From(GuidanceCard card, bool ticked)
    {
        var first = card.Recommendations[0];
        var key = first.Code.Length > 0 ? $"{first.Corpus}:{first.Code}" : first.ChunkId;
        return card.FromDocument
            ? new(new ReflectionReference(key, "", first.Title, "", "Added document" + Pages(card)), ticked)
            : new(new ReflectionReference(
                key, card.Code, first.Title, WebLinks.IsWeb(first.Link) ? first.Link : "",
                card.SourceLabel), ticked);
    }

    public ReflectionReference Stored { get; }

    [ObservableProperty]
    public partial bool Ticked { get; set; }

    public string Key => Stored.Key;

    /// <summary>"NG100", empty for an added document.</summary>
    public string Reference => Stored.Reference;

    /// <summary>The title, empty when it is the same as the reference.</summary>
    public string Title => Stored.Title == Stored.Reference ? "" : Stored.Title;

    /// <summary>"NICE", or "Added document, pages 19 and 21".</summary>
    public string Source => Stored.Source;

    public bool SourceVisible => Stored.Source.Length > 0;

    public string Name => Title.Length > 0 ? $"{Reference} {Title}".Trim() : Reference;

    // The pages the passages came from, as ", page 19" or ", pages 3, 5 and 9"
    private static string Pages(GuidanceCard card)
    {
        var pages = card.Recommendations.Where(r => r.Pages > 0).Select(r => r.Page + 1)
            .Distinct().Order().Select(p => p.ToString(CultureInfo.InvariantCulture))
            .ToList();
        return pages.Count switch
        {
            0 => "",
            1 => $", page {pages[0]}",
            _ => $", pages {string.Join(", ", pages[..^1])} and {pages[^1]}",
        };
    }
}
