using Microsoft.Win32;

namespace ClinicAVT.App.Platform;

/// <summary>
/// WER local dumps for the engine processes. They are per-user and capped, and
/// only minidumps because a full dump could carry audio.
/// </summary>
public static class CrashDumps
{
    private const string LocalDumps =
        @"SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps";

    public static void Register(RegistryKey hive, string dumpFolder, params string[] exeNames)
    {
        foreach (var exe in exeNames)
        {
            try
            {
                using var key = hive.CreateSubKey($@"{LocalDumps}\{exe}");
                key.SetValue("DumpFolder", dumpFolder, RegistryValueKind.ExpandString);
                key.SetValue("DumpCount", 3, RegistryValueKind.DWord);
                key.SetValue("DumpType", 1, RegistryValueKind.DWord);  // minidump
            }
            catch (Exception)
            {
                // Diagnostics must never block startup
            }
        }
    }
}
