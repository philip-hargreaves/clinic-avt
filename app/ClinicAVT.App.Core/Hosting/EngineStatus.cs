namespace ClinicAVT.App.Core.Hosting;

public enum EngineStatus
{
    Stopped,
    Running,
    Restarting,
    Faulted,

    /// <summary>The consultation store was written by a newer version of the app.</summary>
    StoreNewer,

    /// <summary>The consultation store is too old for this version to upgrade.</summary>
    StoreTooOld,
}
