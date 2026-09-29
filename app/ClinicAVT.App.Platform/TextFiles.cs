using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Platform;

public sealed class TextFiles : ITextFiles
{
    public Task WriteAsync(string path, string text) => File.WriteAllTextAsync(path, text);
}
