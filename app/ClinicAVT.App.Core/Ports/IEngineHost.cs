using ClinicAVT.App.Core.Hosting;

namespace ClinicAVT.App.Core.Ports;

/// <summary>StatusChanged may fire on background threads.</summary>
public interface IEngineHost
{
    event Action<EngineStatus>? StatusChanged;

    EngineStatus Status { get; }

    int? EnginePid { get; }

    void Start();

    /// <summary>Asks the engine to exit and stops supervising without waiting, so it can
    /// finish a load after the app closes.</summary>
    Task ReleaseAsync();
}
