using ClinicAVT.App.Core.Common;

namespace ClinicAVT.App.Core.Features.Examples;

/// <summary>A bundled example recording. Display reads "name (m:ss)", or the name alone.</summary>
public sealed record ExampleRecording(string Name, string Path, double Seconds = 0)
{
    public string Display => Math.Round(Seconds) <= 0 ? Name : $"{Name} ({Words.Clock(Seconds)})";
}
