using Microsoft.Extensions.Logging;

namespace ClinicAVT.App.Core.Hosting;

/// <summary>
/// The shell's log lines, one generated method each, so nothing is formatted when logging
/// is off.
/// </summary>
public static partial class Log
{
    [LoggerMessage(Level = LogLevel.Information, Message = "{Line}")]
    public static partial void Line(this ILogger logger, string line);

    [LoggerMessage(Level = LogLevel.Warning, Message = "{Step} failed: {Message}")]
    public static partial void StepFailed(this ILogger logger, string step, string message);

    [LoggerMessage(Level = LogLevel.Warning, Message = "preferences unreadable, using defaults: {Message}")]
    public static partial void PreferencesUnreadable(this ILogger logger, string message);

    [LoggerMessage(Level = LogLevel.Warning, Message = "preferences not saved: {Message}")]
    public static partial void PreferencesNotSaved(this ILogger logger, string message);

    [LoggerMessage(Level = LogLevel.Error, Message = "startup task {Task} failed")]
    public static partial void StartupTaskFailed(this ILogger logger, Exception exception, string task);

    [LoggerMessage(Level = LogLevel.Error, Message = "shutdown step failed")]
    public static partial void ShutdownStepFailed(this ILogger logger, Exception exception);

    [LoggerMessage(Level = LogLevel.Error, Message = "{Handler} failed")]
    public static partial void HandlerFailed(this ILogger logger, Exception exception, string handler);

    [LoggerMessage(Level = LogLevel.Warning, Message = "shown as \"{Shown}\": {Detail}")]
    public static partial void Reworded(this ILogger logger, string shown, string detail);
}
