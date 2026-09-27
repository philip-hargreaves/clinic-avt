namespace ClinicAVT.App.Core.Ports;

/// <summary>Starts one engine process, already bound so it cannot outlive the app.</summary>
public interface IEngineLauncher
{
    IEngineProcess Launch();

    /// <summary>The engine of this install already serving the pipe, as one a closed app
    /// leaves to finish a load. Null when nobody serves it or another program does.</summary>
    IEngineProcess? Adopt();

    /// <summary>Lets the engines launched so far outlive the app, for a clean close.</summary>
    void Release();
}
