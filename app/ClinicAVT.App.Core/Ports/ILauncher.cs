namespace ClinicAVT.App.Core.Ports;

public interface ILauncher
{
    /// <summary>
    /// Opens a web link in the default browser. False for anything but http and https, and
    /// when no browser answered.
    /// </summary>
    Task<bool> OpenLinkAsync(string link);

    /// <summary>A file in its viewer. Throws when no viewer answered.</summary>
    Task OpenFileAsync(string path);

    /// <summary>A folder in the file manager, best effort.</summary>
    void RevealFolder(string path);
}
