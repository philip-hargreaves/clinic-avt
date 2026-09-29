using System.Globalization;

namespace ClinicAVT.App.Core.Common;

public static class Words
{
    public static string Count(int n, string noun) => n == 1 ? $"1 {noun}" : $"{n:N0} {noun}s";

    /// <summary>"m:ss", rounded to the second.</summary>
    public static string Clock(double seconds)
    {
        var whole = (int)Math.Round(seconds);
        return $"{whole / 60}:{whole % 60:00}";
    }

    public static string Position(double seconds) => Clock(Math.Floor(seconds)).PadLeft(5, '0');

    public static DateTimeOffset? LocalTime(string stamp) =>
        DateTimeOffset.TryParse(stamp, CultureInfo.InvariantCulture, out var parsed)
            ? parsed.ToLocalTime()
            : null;

    public static string ShortDate(string stamp) =>
        LocalTime(stamp)?.ToString("d MMM yyyy", CultureInfo.InvariantCulture) ?? "";

    public static string Month(DateTimeOffset when) =>
        when.ToString("MMMM yyyy", CultureInfo.CurrentCulture);

    /// <summary>
    /// "4 Sep" in the given year and "4 Sep 2025" in another. The month is in full when asked.
    /// </summary>
    public static string Day(DateTimeOffset when, int thisYear, bool fullMonth = false)
    {
        var month = fullMonth ? "MMMM" : "MMM";
        return when.ToString(when.Year == thisYear ? $"d {month}" : $"d {month} yyyy", CultureInfo.CurrentCulture);
    }
}
