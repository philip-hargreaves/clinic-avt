using System.Text;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Appraisal;

/// <summary>One reflection as exported, without identifiers and dated to the month.</summary>
public sealed record ReflectionEntry(
    string Title, string Month, string Summary, string Happened, string Learned, string Next)
{
    /// <summary>The guidance the clinician ticked as referred to.</summary>
    public IReadOnlyList<ReflectionReference> References { get; init; } = [];
}

/// <summary>Plain text for appraisal portfolios, which all take free text.</summary>
public static class ReflectionExport
{
    public const string Declaration =
        "The case study was prepared by ClinicAVT from the clinical note and anonymised. "
        + "The reflection is the clinician's own words.";

    public static string Format(ReflectionEntry entry)
    {
        var text = new StringBuilder();
        text.Append(entry.Title.Length > 0 ? entry.Title : "Consultation").Append('\n');
        text.Append(entry.Month).Append("\n\n");
        var hasSummary = entry.Summary.Trim().Length > 0;
        if (hasSummary)
        {
            text.Append("Case study\n").Append(entry.Summary.Trim()).Append("\n\n");
        }

        if (entry.References.Count > 0)
        {
            text.Append("Guidance referred to\n");
            foreach (var reference in entry.References)
            {
                text.Append(Line(reference)).Append('\n');
                if (reference.Link.Length > 0)
                {
                    text.Append(reference.Link).Append('\n');
                }
            }

            text.Append('\n');
        }

        Section(text, "What stood out?", entry.Happened);
        Section(text, "What did I learn?", entry.Learned);
        Section(text, "Would I do anything differently?", entry.Next);
        if (hasSummary)
        {
            text.Append(Declaration).Append('\n');
        }

        return text.ToString().TrimEnd() + "\n";
    }

    // "NG100 Rheumatoid arthritis in adults (NICE)"
    private static string Line(ReflectionReference reference)
    {
        var name = reference.Title.Length > 0 && reference.Title != reference.Reference
            ? $"{reference.Reference} {reference.Title}".Trim()
            : reference.Reference.Length > 0 ? reference.Reference : reference.Title;
        return reference.Source.Length > 0 ? $"{name} ({reference.Source})" : name;
    }

    private static void Section(StringBuilder text, string question, string answer)
    {
        if (answer.Trim().Length == 0)
        {
            return;
        }

        text.Append(question).Append('\n').Append(answer.Trim()).Append("\n\n");
    }
}
