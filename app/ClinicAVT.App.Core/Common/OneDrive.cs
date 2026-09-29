using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Common;

public static class OneDrive
{
    // A folder under a OneDrive root syncs to the cloud, which the caption states
    public static bool Holds(string folder, IEnumerable<string> roots) =>
        folder.Length > 0
        && roots.Any(root => folder.StartsWith(root, StringComparison.OrdinalIgnoreCase));

    /// <summary>Every root that is set, the default account's first.</summary>
    public static IEnumerable<string> Roots(this IOneDriveFolders folders) =>
        new[] { folders.Primary, folders.Personal, folders.Work }
            .Where(root => !string.IsNullOrEmpty(root))
            .Select(root => root!);
}
