namespace ClinicAVT.App.Core.Ports;

public interface IClipboard
{
    /// <summary>False when the clipboard would not take the text.</summary>
    Task<bool> CopyAsync(string text);
}
