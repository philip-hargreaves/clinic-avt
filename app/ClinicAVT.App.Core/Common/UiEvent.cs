using System.Diagnostics;
using System.Runtime.CompilerServices;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Hosting;

namespace ClinicAVT.App.Core.Common;

/// <summary>
/// Runs the async work for a UI event. An exception from an async void handler would end the
/// app, so failures are logged and swallowed. Any message for the clinician comes from the view
/// model.
/// </summary>
public static class UiEvent
{
    /// <summary>Set once at startup.</summary>
    public static ILogger? Logger { get; set; }

    /// <summary>
    /// Starts the work and returns at its first await. Failures are logged under the calling
    /// handler, such as "ReflectionEditorView.OnTitleCommitted".
    /// </summary>
    public static async void Run(Func<Task> work, [CallerFilePath] string file = "",
        [CallerMemberName] string handler = "") =>
        await RunAsync(work, Logger, $"{Path.GetFileName(file).Split('.')[0]}.{handler}")
            .ConfigureAwait(true);

    /// <summary>Runs the work and logs any failure without throwing.</summary>
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
