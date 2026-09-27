namespace ClinicAVT.App.Core.Hosting;

/// <summary>Shifts engine.log to engine-1.log and so on, keeping the last
/// <c>keep</c>. Runs are appended until the file passes <c>atBytes</c>, so a burst of launches
/// cannot rotate a crash's log away.</summary>
public static class LogRotation
{
    public const long RotateAtBytes = 2 * 1024 * 1024;

    public static void Rotate(string path, int keep, long atBytes = RotateAtBytes)
    {
        try
        {
            var file = new FileInfo(path);
            if (!file.Exists || file.Length < atBytes)
            {
                return;
            }

            File.Delete(Shifted(path, keep - 1));
            for (var i = keep - 2; i >= 1; i--)
            {
                if (File.Exists(Shifted(path, i)))
                {
                    File.Move(Shifted(path, i), Shifted(path, i + 1), overwrite: true);
                }
            }

            File.Move(path, Shifted(path, 1), overwrite: true);
        }
        catch (Exception)
        {
            // A locked or missing log must never block the engine launch
        }
    }

    private static string Shifted(string path, int index) => Path.Combine(
        Path.GetDirectoryName(path) ?? "",
        $"{Path.GetFileNameWithoutExtension(path)}-{index}{Path.GetExtension(path)}");
}
