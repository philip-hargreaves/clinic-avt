namespace ClinicAVT.App.Core.Ports;

/// <summary>The OneDrive roots Windows reports, each null when not set.</summary>
public interface IOneDriveFolders
{
    string? Work { get; }

    string? Personal { get; }

    /// <summary>The default account's root.</summary>
    string? Primary { get; }
}
