using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Platform;

/// <summary>The OneDrive roots from the variables the OneDrive client sets, read on each call.</summary>
public sealed class OneDriveFolders : IOneDriveFolders
{
    public string? Work => Environment.GetEnvironmentVariable("OneDriveCommercial");

    public string? Personal => Environment.GetEnvironmentVariable("OneDriveConsumer");

    public string? Primary => Environment.GetEnvironmentVariable("OneDrive");
}
