namespace ClinicAVT.App.Core.Ports;

public interface IClipboard
{
    /// <summary>False when the clipboard would not take the text.</summary>
    Task<bool> CopyAsync(string text);

    /// <summary>
    /// Copies a secret such as a backup password, kept out of clipboard history and off the
    /// clinician's other devices. False when the clipboard would not take it.
    /// </summary>
    Task<bool> CopySecretAsync(string text);
}
