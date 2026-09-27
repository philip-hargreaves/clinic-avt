using ClinicAVT.App.Core.Hosting;

namespace ClinicAVT.App.Core.Ports;

/// <summary>
/// The shell's port to the engine process lifecycle. StatusChanged may fire on
/// background threads.
/// </summary>
public interface IEngineHost
{
    event Action<EngineStatus>? StatusChanged;

    EngineStatus Status { get; }

    int? EnginePid { get; }

    void Start();

    /// <summary>Asks the engine to leave and stops supervising it without waiting, so it can
    /// finish a load after the app has closed.</summary>
    Task ReleaseAsync();
}
