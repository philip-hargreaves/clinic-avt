namespace ClinicAVT.App.Core.Hosting;

/// <summary>The only place that asks for the build configuration.</summary>
public static class BuildFlags
{
    /// <summary>Developer-only surfaces show in a debug build and never ship.</summary>
#if DEBUG
    public static bool Debug => true;
#else
    public static bool Debug => false;
#endif
}
