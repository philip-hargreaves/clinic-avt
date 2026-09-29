using ClinicAVT.App.Core.Metrics;

namespace ClinicAVT.App.Core.Ports;

/// <summary>Hardware identity for the performance report, queried once.</summary>
public interface IMachineInfoProvider
{
    MachineInfo Describe();

    /// <summary>The computer's network name.</summary>
    string MachineName { get; }
}
