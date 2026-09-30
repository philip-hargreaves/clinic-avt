using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Common;

/// <summary>Runs engine calls and turns a failure into one line of text.</summary>
public static class EngineCall
{
    /// <summary>
    /// Shows a failure on the status line as "{problem}: {reason}", with the engine's reason in
    /// plain words, and logs the detail. Cancellation still throws.
    /// </summary>
    public static async Task<bool> ReportAsync(IStatusLine status, string problem, Func<Task> call) =>
        await ReportAsync(status, problem, async () =>
        {
            await call().ConfigureAwait(true);
            return true;
        }).ConfigureAwait(true);

    /// <summary>The call's value, or null when it failed.</summary>
    public static async Task<T?> ReportAsync<T>(IStatusLine status, string problem, Func<Task<T>> call)
    {
        try
        {
            return await call().ConfigureAwait(true);
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            status.Log($"{problem}: {e.Message}");
            status.Append($"{problem}: {EngineWords.Reason(e)}");
            return default;
        }
    }

    /// <summary>
    /// Any failure, cancellation included, goes to the log as "{step} failed: {reason}".
    /// </summary>
    public static async Task<bool> LogAsync(IStatusLine status, string step, Func<Task> call) =>
        await LogAsync(status, step, async () =>
        {
            await call().ConfigureAwait(true);
            return true;
        }).ConfigureAwait(true);

    /// <summary>The call's value, or null when it failed.</summary>
    public static async Task<T?> LogAsync<T>(IStatusLine status, string step, Func<Task<T>> call)
    {
        try
        {
            return await call().ConfigureAwait(true);
        }
        catch (Exception e)
        {
            status.Log($"{step} failed: {e.Message}");
            return default;
        }
    }

    /// <summary>For optional steps. Reports and logs a failure without throwing.</summary>
    public static async Task<bool> TryAsync(IStatusLine status, string step, Func<Task> call) =>
        await TryAsync(status, step, async () =>
        {
            await call().ConfigureAwait(true);
            return true;
        }).ConfigureAwait(true);

    /// <summary>
    /// The call's value, or null when it failed. If refused is set, an engine refusal shows on the
    /// status line as "{refused}: {reason}".
    /// </summary>
    public static async Task<T?> TryAsync<T>(
        IStatusLine status, string step, Func<Task<T>> call, string? refused = null)
    {
        try
        {
            return await call().ConfigureAwait(true);
        }
        catch (OperationCanceledException)
        {
            status.Append("Taking longer than expected", busy: true);
            return default;
        }
        catch (Exception e)
        {
            status.Log($"{step} failed: {e.Message}");
            var explained = e is EngineErrorException engine
                && engine.Code is Protocol.SessionErrorCode or Protocol.CaptureFailedCode;
            status.Append(refused is not null && explained
                ? $"{refused}: {EngineWords.Reason(e)}"
                : "A step failed - trying to continue");
            return default;
        }
    }
}
