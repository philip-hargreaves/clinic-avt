using System.Globalization;
using ClinicAVT.App.Core.Common;

namespace ClinicAVT.App.Core.Features.Documents;

/// <summary>
/// "Edited 10:31" when the edit fell on the consultation's own day,
/// "Edited 27 Aug" otherwise, "" when never edited.
/// </summary>
public static class EditedStamp
{
    public static string Label(string sessionDayAt, string editedAt)
    {
        if (Words.LocalTime(editedAt) is not { } local)
        {
            return "";
        }

        var sameDay = Words.LocalTime(sessionDayAt) is { } day && day.Date == local.Date;
        return "Edited " + (sameDay
            ? local.ToString("HH:mm", CultureInfo.CurrentCulture)
            : local.ToString("d MMM", CultureInfo.CurrentCulture));
    }

    public static string Now(TimeProvider time) =>
        "Edited " + time.GetLocalNow().ToString("HH:mm", CultureInfo.CurrentCulture);
}
