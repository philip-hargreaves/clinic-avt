using ClinicAVT.App.Core.Metrics;

namespace ClinicAVT.App.Core.Ports;

public interface IPowerStateReader
{
    PowerState Read();
}
