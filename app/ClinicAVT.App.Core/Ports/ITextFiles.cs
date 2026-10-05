namespace ClinicAVT.App.Core.Ports;

/// <summary>Plain text files outside the encrypted store.</summary>
public interface ITextFiles
{
    /// <summary>
    /// Writes the text, replacing the file. Throws IOException or UnauthorizedAccessException.
    /// </summary>
    Task WriteAsync(string path, string text);
}
