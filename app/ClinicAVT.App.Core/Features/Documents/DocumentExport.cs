namespace ClinicAVT.App.Core.Features.Documents;

public static class DocumentExport
{
    /// <summary>
    /// First line of an exported note or sheet. The engine's patient-data screen refuses any file
    /// carrying it, so an export dropped in the guidelines folder is never searched.
    /// </summary>
    public const string Marker = "ClinicAVT export - not for the guidelines folder.\n\n";
}
