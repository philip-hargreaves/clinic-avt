namespace ClinicAVT.App.Core.Features.Sessions;

public sealed record SessionRow(
    string Id, string Title, string Started, string Duration, string EditedLabel,
    string StartedAt = "", bool Sample = false, bool HasReflection = false)
{
    public bool Edited => EditedLabel.Length > 0;

    /// <summary>An unlabelled row shows the date once.</summary>
    public bool HasLabel => Title != Started;

    public string Heading => HasLabel ? Title : $"{Started} · {Duration}";

    public string Meta => HasLabel ? $"{Started} · {Duration}" : "";

    public bool MetaVisible => HasLabel || Edited;
}
