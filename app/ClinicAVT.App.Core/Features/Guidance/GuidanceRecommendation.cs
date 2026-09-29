using ClinicAVT.App.Core.Common;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Guidance;

public sealed record GuidanceRecommendation(
    string Corpus, string ChunkId, string Code, string Number, string Title, string Section,
    string Text, string Link, string LastUpdated, string UpdateTag, string Source,
    string Citation, string Trigger, string SourceLabel, bool FromNote,
    long Document = 0, int Page = 0, int Pages = 0, bool Labelled = false)
{
    /// <summary>Labelled when the corpus manifest names its publisher for the chip.</summary>
    public static GuidanceRecommendation From(
        GuidanceResult result, string sourceLabel, bool fromNote, bool labelled = false) =>
        new(
            result.Corpus ?? "", result.ChunkId ?? "", result.Code ?? "", result.Number ?? "",
            result.Title ?? "", result.Section ?? "", (result.Text ?? "").TrimEnd(),
            result.Url ?? "", result.LastUpdated ?? "", result.UpdateTag ?? "", result.Source ?? "",
            result.Citation ?? "", result.Trigger ?? "", sourceLabel, fromNote,
            result.Document, result.Page, result.Pages, labelled);

    /// <summary>
    /// Every shown result. The source label needs the corpus name, which only the searched
    /// list carries.
    /// </summary>
    public static List<GuidanceRecommendation> ReadAll(GuidanceRecord record, bool fromNote)
    {
        var sources = new Dictionary<string, (string Name, string Label)>(StringComparer.Ordinal);
        foreach (var corpus in record.Searched)
        {
            sources[corpus.Id] = (corpus.Name, corpus.Label ?? "");
        }

        var results = new List<GuidanceRecommendation>();
        foreach (var result in record.Shown)
        {
            var (name, label) = sources.GetValueOrDefault(result.Corpus ?? "", ("", ""));
            var labelled = label.Length > 0;
            results.Add(From(result, labelled ? label : name, fromNote, labelled));
        }

        return results;
    }

    /// <summary>A passage from a document the clinician added.</summary>
    public bool FromDocument => Source == "upload";

    public string PageLabel => Pages > 0 ? $"Page {Page + 1}" : "";

    public bool PageLabelVisible => PageLabel.Length > 0;

    /// <summary>
    /// "NG100 1.1.1", the code alone, or for a document its number, its page or its title.
    /// </summary>
    public string Reference => Code.Length > 0
        ? Number.Length > 0 ? $"{Code.ToUpperInvariant()} {Number}" : Code.ToUpperInvariant()
        : Number.Length > 0 ? Number
        : PageLabel.Length > 0 ? PageLabel
        : Title;

    /// <summary>"[2009, amended 2018]" as the guideline marks it, empty when unmarked.</summary>
    public string Tag => UpdateTag.Length > 0 ? $"[{UpdateTag}]" : "";

    public bool TagVisible => UpdateTag.Length > 0;

    public string Path => Section.Replace(" > ", " › ", StringComparison.Ordinal);

    public bool PathVisible => Section.Length > 0;

    public bool LabelVisible =>
        Number.Length > 0 || Section.Length > 0 || UpdateTag.Length > 0 || PageLabelVisible;

    /// <summary>
    /// The note sentence that found it, or the whole note. A typed query has no line.
    /// </summary>
    public string Matched => Trigger.Length > 0 ? $"Matched: “{Trigger}”"
        : FromNote ? "Matched: the note as a whole"
        : "";

    public bool MatchedVisible => Matched.Length > 0;

    /// <summary>A web link opens in the browser, a document as a copy in the PDF viewer.</summary>
    public bool CanOpen => FromDocument || HasWebLink;

    /// <summary>Plain-text corpora put a file name in Link.</summary>
    private bool HasWebLink => WebLinks.IsWeb(Link);

    public string CitationText => HasWebLink ? $"{Citation}\n{Link}" : Citation;

    public string OpenTip => FromDocument ? "Opens the file in your PDF viewer." : "";

    /// <summary>The Show in document button, for a document with pages.</summary>
    public bool ShowVisible => FromDocument && Pages > 0;

    public string ShowName => $"Show {Reference} in the document";

    public string OpenName => FromDocument ? $"Open {Title}" : $"Open {Reference}";

    public string CopyName => $"Copy citation for {Reference}";
}
