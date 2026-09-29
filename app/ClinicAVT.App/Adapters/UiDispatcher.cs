using Microsoft.UI.Dispatching;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Adapters;

/// <summary>
/// Captures the queue of the thread that first resolves it. That is the UI thread during launch.
/// </summary>
public sealed class UiDispatcher : IUiDispatcher
{
    private readonly DispatcherQueue _queue = DispatcherQueue.GetForCurrentThread();

    public void Post(Action action)
    {
        if (!_queue.TryEnqueue(() => action()))
        {
            throw new InvalidOperationException("UI dispatcher queue rejected the work item");
        }
    }
}
