namespace ClinicAVT.App.Core.Ports;

/// <summary>
/// Runs an action on the UI thread. Engine events arrive on a background thread and bound state is
/// UI-thread only.
/// </summary>
public interface IUiDispatcher
{
    void Post(Action action);
}
