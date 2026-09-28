using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Common;

/// <summary>Runs engine calls and turns a failure into one line of text.</summary>
public static class EngineCall
{
    /// <summary>
    /// A failure goes on the status line as "{problem}: {reason}", the engine's reason in plain
    /// words, and its detail to the log. Cancellation still throws.
    /// </summary>
    public static async Task<bool> ReportAsync(StatusBarViewModel? status, string problem, Func<Task> call) =>
        await ReportAsync(status, problem, async () =>
        {
            await call().ConfigureAwait(true);
            return true;
        }).ConfigureAwait(true);

    /// <summary>The call's value, or null when it failed.</summary>
    public static async Task<T?> ReportAsync<T>(StatusBarViewModel? status, string problem, Func<Task<T>> call)
    {
        try
        {
            return await call().ConfigureAwait(true);
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            status?.Log($"{problem}: {e.Message}");
            status?.Append($"{problem}: {EngineWords.Reason(e)}");
            return default;
        }
    }

    /// <summary>
    /// Any failure, cancellation included, goes to the log as "{step} failed: {reason}".
    /// </summary>
    public static async Task<bool> LogAsync(StatusBarViewModel? status, string step, Func<Task> call) =>
        await LogAsync(status, step, async () =>
        {
            await call().ConfigureAwait(true);
            return true;
        }).ConfigureAwait(true);

    /// <summary>The call's value, or null when it failed.</summary>
    public static async Task<T?> LogAsync<T>(StatusBarViewModel? status, string step, Func<Task<T>> call)
    {
        try
        {
            return await call().ConfigureAwait(true);
        }
        catch (Exception e)
        {
            status?.Log($"{step} failed: {e.Message}");
            return default;
        }
    }

    /// <summary>
    /// A step inside a flow that carries on when it fails. The failure is reported on the
    /// status line and logged, and nothing is thrown.
    /// </summary>
    public static async Task<bool> TryAsync(StatusBarViewModel status, string step, Func<Task> call) =>
        await TryAsync(status, step, async () =>
        {
            await call().ConfigureAwait(true);
            return true;
        }).ConfigureAwait(true);

    /// <summary>
    /// The call's value, or null when it failed. With refused, the engine's reason for a refusal
    /// goes on the status line as "{refused}: {reason}".
    /// </summary>
    public static async Task<T?> TryAsync<T>(
        StatusBarViewModel status, string step, Func<Task<T>> call, string? refused = null)
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
            status.Append(refused is not null && e is EngineErrorException { Code: Protocol.SessionErrorCode }
                ? $"{refused}: {EngineWords.Reason(e)}"
                : "A step failed - trying to continue");
            return default;
        }
    }
}
