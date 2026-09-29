using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Platform;

public sealed class FileMetricsLog(string path) : IMetricsLog
{
    public void Append(string line)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.AppendAllText(path, line + Environment.NewLine);
    }

    public IReadOnlyList<string>? ReadAll() => File.Exists(path) ? File.ReadAllLines(path) : null;
}
