using System.Text.RegularExpressions;

namespace ClinicAVT.App.Core.Features.Appraisal;

/// <summary>
/// Patterns that may identify a patient. It reports what it finds without editing the text,
/// and it cannot catch everything.
/// </summary>
public static partial class IdentifierCheck
{
    public static IReadOnlyList<string> Find(string text)
    {
        var found = new List<string>();
        void Add(string kind, Regex pattern)
        {
            foreach (Match match in pattern.Matches(text))
            {
                found.Add($"{kind}: {match.Value.Trim()}");
            }
        }

        Add("a name", TitledName());
        Add("a date", NumericDate());
        Add("a date", WrittenDate());
        Add("an exact age", ExactAge());
        Add("an NHS number", NhsNumber());
        Add("a postcode", Postcode());
        Add("a phone number", PhoneNumber());
        return found;
    }

    /// <summary>One line for the caption under a text, empty when nothing was found.</summary>
    public static string Describe(string text)
    {
        var found = Find(text);
        return found.Count == 0
            ? ""
            : "This may identify the patient: " + string.Join("; ", found.Take(3))
              + (found.Count > 3 ? $" and {found.Count - 3} more" : "");
    }

    [GeneratedRegex(@"\b(?:Mr|Mrs|Ms|Miss|Mx|Dr|Prof)\.?\s+[A-Z][a-z]+(?:\s+[A-Z][a-z]+)?")]
    private static partial Regex TitledName();

    [GeneratedRegex(@"\b\d{1,2}[/.\-]\d{1,2}[/.\-]\d{2,4}\b")]
    private static partial Regex NumericDate();

    // The two forms with a day, which the engine's scrub removes. A month with or without its
    // year is allowed because the entry itself is dated to the month
    [GeneratedRegex(
        @"\b(?:\d{1,2}(?:st|nd|rd|th)?\s+(?:of\s+)?(?:January|February|March|April|May|June|July|August|September|October|November|December|Jan|Feb|Mar|Apr|Jun|Jul|Aug|Sept|Sep|Oct|Nov|Dec)\b\.?(?:,?\s+\d{4})?|(?:January|February|March|April|May|June|July|August|September|October|November|December|Jan|Feb|Mar|Apr|Jun|Jul|Aug|Sept|Sep|Oct|Nov|Dec)\b\.?\s+\d{1,2}(?:st|nd|rd|th)?(?:,?\s+\d{4})?\b)")]
    private static partial Regex WrittenDate();

    // Every spelling the engine's scrub removes
    [GeneratedRegex(@"\b(?:(?:aged?|age of)\s+\d{1,3}|\d{1,3}(?:[ -]?(?:year|yr)s?[ -]old|\s*y/?o))\b",
        RegexOptions.IgnoreCase)]
    private static partial Regex ExactAge();

    [GeneratedRegex(@"\b\d{3}[ \-]?\d{3}[ \-]?\d{4}\b")]
    private static partial Regex NhsNumber();

    [GeneratedRegex(@"\b[A-Z]{1,2}\d[A-Z\d]?\s*\d[A-Z]{2}\b")]
    private static partial Regex Postcode();

    [GeneratedRegex(@"\b0\d{2,4}[ \-]?\d{3,4}[ \-]?\d{3,4}\b")]
    private static partial Regex PhoneNumber();
}
