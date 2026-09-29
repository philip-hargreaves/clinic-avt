namespace ClinicAVT.App.Core.Ports;

/// <summary>Starts one engine process, already bound so it cannot outlive the app.</summary>
public interface IEngineLauncher
{
    IEngineProcess Launch();

    /// <summary>This install's engine if it already serves the pipe, e.g. one a closed app left
    /// finishing a load. Null if there is none, or if another program holds the pipe.</summary>
    IEngineProcess? Adopt();

    /// <summary>Lets the engines launched so far outlive the app, for a clean close.</summary>
    void Release();
}
