using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Features.Help;

/// <summary>The guide itself lives in markup. This holds the small print about the build.</summary>
public sealed class HelpViewModel(IAppInfo app)
{
    public string VersionLine { get; } =
        $"Version {app.Version} · Developed by Philip Hargreaves in collaboration with UCL, Intel and the NHS";
}
