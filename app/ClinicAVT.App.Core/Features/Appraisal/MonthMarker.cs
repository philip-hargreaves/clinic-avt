using ClinicAVT.App.Core.Common;

namespace ClinicAVT.App.Core.Features.Appraisal;

public sealed record MonthMarker(int Month, string Name, int Count, bool Current = false, bool Selected = false)
{
    public bool Filled => Count > 0;

    public string Tip => Count == 0 ? "No reflections"
        : Selected ? "Show the whole year"
        : $"{Words.Count(Count, "reflection")}, press to show only this month";
}
