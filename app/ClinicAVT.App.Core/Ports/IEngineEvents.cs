using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Ports;

/// <summary>Engine notifications and connection changes, delivered on the UI thread.</summary>
public interface IEngineEvents
{
    bool Connected { get; }

    /// <summary>Calls the handler for each notification of that type. Dispose to stop.</summary>
    IDisposable Subscribe<T>(Action<T> handler)
        where T : EngineNotification;

    /// <summary>Calls the handler on every connect and disconnect. Dispose to stop.</summary>
    IDisposable SubscribeConnection(Action<bool> handler);
}
