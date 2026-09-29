using ClinicAVT.App.Core.Metrics;

namespace ClinicAVT.App.Core.Ports;

/// <summary>Hardware identity for the performance report, queried once.</summary>
public interface IMachineInfoProvider
{
    MachineInfo Describe();

    string MachineName { get; }
}
