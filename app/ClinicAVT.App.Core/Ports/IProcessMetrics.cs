namespace ClinicAVT.App.Core.Ports;

/// <summary>
/// Memory figures of the product's processes, for the metrics log and the memory chip.
/// </summary>
public interface IProcessMetrics
{
    long? PeakWorkingSetMb(int pid);

    long? PeakCommitMb(int pid);

    /// <summary>The largest peak among the processes of that name, null when none runs.</summary>
    long? PeakWorkingSetMbOf(string processName);

    /// <summary>
    /// Memory held by this process plus every process of the named images, in GB: committed
    /// private bytes, which an idle process keeps when Windows trims its working set.
    /// </summary>
    double CommittedGb(params string[] processNames);
}
