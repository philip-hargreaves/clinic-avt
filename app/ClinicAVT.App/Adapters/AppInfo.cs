using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Adapters;

/// <summary>The shell assembly's version as major.minor.build.</summary>
public sealed class AppInfo : IAppInfo
{
    public string Version { get; } = Describe(typeof(AppInfo).Assembly.GetName().Version);

    private static string Describe(Version? version)
    {
        version ??= new Version(0, 0, 0);
        return $"{version.Major}.{version.Minor}.{version.Build}";
    }
}
