namespace ClinicAVT.TestSupport;

/// <summary>
/// Finds the engine binary for real-engine tests. CLINICAVT_ENGINE_PATH comes first, then the
/// release build, then the newest build under any preset. Release matches the shipped app, and
/// OpenVINO's debug GPU plugin asserts on the second note/patient generation.
/// </summary>
internal static class EnginePath
{
    public static string Find()
    {
        var overridePath = Environment.GetEnvironmentVariable("CLINICAVT_ENGINE_PATH");
        if (!string.IsNullOrEmpty(overridePath))
        {
            return overridePath;
        }

        const string exe = "clinicavt_engine.exe";
        for (var dir = AppContext.BaseDirectory; dir is not null; dir = Path.GetDirectoryName(dir))
        {
            var buildRoot = Path.Combine(dir, "build");
            if (!Directory.Exists(buildRoot))
            {
                continue;
            }

            var release = Path.Combine(buildRoot, "release", "engine", exe);
            if (File.Exists(release))
            {
                return release;
            }

            var newest = Directory
                .EnumerateFiles(buildRoot, exe, SearchOption.AllDirectories)
                .Select(path => new FileInfo(path))
                .OrderByDescending(info => info.LastWriteTimeUtc)
                .FirstOrDefault();
            if (newest is not null)
            {
                return newest.FullName;
            }
        }

        throw new FileNotFoundException(
            $"{exe} not found, build it with: cmake --workflow --preset release");
    }

    /// <summary>The staged models folder above the test output, null when none is staged.</summary>
    public static string? FindModels()
    {
        for (var dir = AppContext.BaseDirectory; dir is not null; dir = Path.GetDirectoryName(dir))
        {
            var models = Path.Combine(dir, "models");
            if (Directory.Exists(models))
            {
                return models;
            }
        }

        return null;
    }
}
