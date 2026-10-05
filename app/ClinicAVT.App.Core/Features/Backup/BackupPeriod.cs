using System.Globalization;

namespace ClinicAVT.App.Core.Features.Backup;

/// <summary>
/// Whole local days, First to Last inclusive. A null end is open, and Everything has both
/// ends null. Sent to the engine as a half-open UTC range between local midnights.
/// </summary>
public sealed record BackupPeriod(DateOnly? First, DateOnly? Last)
{
    public static readonly BackupPeriod Everything = new(null, null);

    /// <summary>The options in the dialog's order, with Everything the default.</summary>
    public static readonly IReadOnlyList<string> Kinds =
        ["This month", "Last month", "Last 3 months", "Everything", "Choose dates"];

    public const int EverythingIndex = 3;

    public const int ChooseDatesIndex = 4;

    /// <summary>
    /// The option at that index on the given day. Choose dates uses the two picked days.
    /// </summary>
    public static BackupPeriod For(int index, DateOnly today, DateOnly? from = null, DateOnly? to = null)
    {
        var month = new DateOnly(today.Year, today.Month, 1);
        return index switch
        {
            0 => new(month, month.AddMonths(1).AddDays(-1)),
            1 => new(month.AddMonths(-1), month.AddDays(-1)),
            2 => new(month.AddMonths(-2), month.AddMonths(1).AddDays(-1)),
            ChooseDatesIndex => new(from, to),
            _ => Everything,
        };
    }

    /// <summary>The option's label with its real months, such as "Last month (August)".</summary>
    public static string Label(int index, DateOnly today)
    {
        var period = For(index, today);
        return index switch
        {
            0 or 1 => $"{Kinds[index]} ({MonthName(period.First!.Value)})",
            2 => $"{Kinds[index]} ({MonthName(period.First!.Value)} to {MonthName(period.Last!.Value)})",
            _ => Kinds[index],
        };
    }

    public bool Valid => First is null || Last is null || First <= Last;

    public string From(TimeZoneInfo zone) => First is { } day ? Utc(day, zone) : "";

    public string To(TimeZoneInfo zone) => Last is { } day ? Utc(day.AddDays(1), zone) : "";

    /// <summary>
    /// "1 Jul to 30 Sep 2026", the year once when both ends share it. Empty for Everything.
    /// </summary>
    public string Span() => (First, Last) switch
    {
        ({ } first, { } last) when first.Year == last.Year =>
            $"{first.ToString("d MMM", CultureInfo.InvariantCulture)} to {Day(last)}",
        ({ } first, { } last) => $"{Day(first)} to {Day(last)}",
        ({ } first, null) => $"from {Day(first)}",
        (null, { } last) => $"up to {Day(last)}",
        _ => "",
    };

    /// <summary>True when the moment falls on one of the period's local days.</summary>
    public bool Holds(DateTimeOffset when, TimeZoneInfo zone)
    {
        var day = DateOnly.FromDateTime(TimeZoneInfo.ConvertTime(when, zone).DateTime);
        return (First is null || day >= First) && (Last is null || day <= Last);
    }

    /// <summary>
    /// The file name without its extension, such as "ClinicAVT backup 1 Aug to 31 Aug 2026",
    /// dated by what it holds.
    /// </summary>
    public string FileName(DateOnly today, string kind = "backup") =>
        First is null && Last is null ? $"ClinicAVT {kind} {Day(today)}" : $"ClinicAVT {kind} {Span()}";

    /// <summary>
    /// A period the engine gave back, from its half-open UTC ends, in local days.
    /// </summary>
    public static BackupPeriod FromWire(string? from, string? to, TimeZoneInfo zone) =>
        new(LocalDay(from, zone), LocalDay(to, zone)?.AddDays(-1));

    private static string Day(DateOnly day) => day.ToString("d MMM yyyy", CultureInfo.InvariantCulture);

    private static string MonthName(DateOnly day) => day.ToString("MMMM", CultureInfo.InvariantCulture);

    private static string Utc(DateOnly day, TimeZoneInfo zone)
    {
        var midnight = day.ToDateTime(TimeOnly.MinValue);
        var utc = new DateTimeOffset(midnight, zone.GetUtcOffset(midnight)).UtcDateTime;
        return utc.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture);
    }

    private static DateOnly? LocalDay(string? stamp, TimeZoneInfo zone) =>
        DateTimeOffset.TryParse(stamp, CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal, out var parsed)
            ? DateOnly.FromDateTime(TimeZoneInfo.ConvertTime(parsed, zone).DateTime)
            : null;
}
