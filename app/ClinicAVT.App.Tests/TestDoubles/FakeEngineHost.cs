using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Tests.TestDoubles;

/// <summary>An engine host whose status the test moves by hand.</summary>
public sealed class FakeEngineHost : IEngineHost
{
    public event Action<EngineStatus>? StatusChanged;

    public EngineStatus Status { get; private set; } = EngineStatus.Stopped;

    public int? EnginePid { get; set; }

    public void Start()
    {
        RaiseStatus(EngineStatus.Running);
    }

    public Task ReleaseAsync()
    {
        RaiseStatus(EngineStatus.Stopped);
        return Task.CompletedTask;
    }

    public void RaiseStatus(EngineStatus status)
    {
        Status = status;
        StatusChanged?.Invoke(status);
    }
}
