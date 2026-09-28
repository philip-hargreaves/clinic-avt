using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Metrics;

/// <summary>Reports no figures. Tests and collectors without a reader use it.</summary>
public sealed class NoProcessMetrics : IProcessMetrics
{
    public long? PeakWorkingSetMb(int pid) => null;

    public long? PeakCommitMb(int pid) => null;

    public long? PeakWorkingSetMbOf(string processName) => null;

    public double CommittedGb(params string[] processNames) => 0;
}
