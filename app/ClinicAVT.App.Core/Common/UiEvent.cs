using System.Diagnostics;
using System.Runtime.CompilerServices;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Hosting;

namespace ClinicAVT.App.Core.Common;

/// <summary>
/// Runs the async work behind a UI event. An exception out of an async void handler ends the
/// app, so a failure is logged and goes no further. The clinician sees only what the view model
/// already shows.
/// </summary>
public static class UiEvent
{
    /// <summary>Where failures go, set once at startup.</summary>
    public static ILogger? Logger { get; set; }

    /// <summary>
    /// Starts the work from an event handler and returns at its first await. The log names the
    /// handler as "ReflectionEditorView.OnTitleCommitted".
    /// </summary>
    public static async void Run(Func<Task> work, [CallerFilePath] string file = "",
        [CallerMemberName] string handler = "") =>
        await RunAsync(work, Logger, $"{Path.GetFileName(file).Split('.')[0]}.{handler}")
            .ConfigureAwait(true);

    /// <summary>The work, a failure logged rather than thrown.</summary>
    public static async Task RunAsync(Func<Task> work, ILogger? logger, string handler)
    {
        try
        {
            await work().ConfigureAwait(true);
        }
        catch (Exception e)
        {
            if (logger is null)
            {
                Trace.TraceError($"{handler} failed: {e}");
            }
            else
            {
                logger.HandlerFailed(e, handler);
            }
        }
    }
}
