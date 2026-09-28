using System.Globalization;
using ClinicAVT.App.Core.Preferences;

namespace ClinicAVT.App.Core.Common;

/// <summary>
/// The words a consultation is listed and headed with, shared by the inbox and the live
/// screen.
/// </summary>
public static class SessionText
{
    /// <summary>"25 Sep 05:27" in local time, or the raw value when it does not parse.</summary>
    public static string Started(string startedAt) =>
        Words.LocalTime(startedAt) is { } started ? Started(started) : startedAt;

    public static string Started(DateTimeOffset started) =>
        started.ToLocalTime().ToString("d MMM HH:mm", CultureInfo.CurrentCulture);

    /// <summary>
    /// "Thursday 25 September, 15:09", the heading over a consultation just recorded.
    /// </summary>
    public static string Heading(DateTimeOffset started) =>
        started.ToLocalTime().ToString("dddd d MMMM, HH:mm", CultureInfo.CurrentCulture);

    /// <summary>
    /// "12 min". The audio length is the consultation's length. The wall clock is only the
    /// fallback, because an import stores minutes of audio in seconds.
    /// </summary>
    public static string Duration(double audioSeconds, string startedAt = "", string endedAt = "")
    {
        var seconds = audioSeconds;
        if (seconds <= 0)
        {
            if (Words.LocalTime(startedAt) is not { } started || Words.LocalTime(endedAt) is not { } ended)
            {
                return "";
            }

            seconds = (ended - started).TotalSeconds;
        }

        return seconds < 90 ? "1 min" : $"{(int)Math.Round(seconds / 60)} min";
    }

    /// <summary>"SOAP, detailed", the options the note was written with.</summary>
    public static string Options(string style, string detail) =>
        $"{NoteOptions.Style(style).Name}, {NoteOptions.Detail(detail).Name.ToLowerInvariant()}";

    /// <summary>The parts that are known, dotted together.</summary>
    public static string Meta(params string[] parts) =>
        string.Join(" · ", parts.Where(p => p.Length > 0));
}
