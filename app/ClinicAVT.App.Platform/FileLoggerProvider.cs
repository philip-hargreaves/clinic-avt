using System.Globalization;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Hosting;

namespace ClinicAVT.App.Platform;

/// <summary>
/// The shell's diagnostics log beside the engine's log, one line per event. Launches append
/// until the file is large, so a crash's lines survive many starts.
/// </summary>
public sealed class FileLoggerProvider : ILoggerProvider
{
    private readonly object _gate = new();
    private readonly string _path;
    private StreamWriter? _writer;

    public FileLoggerProvider(string path, int keep = 5)
    {
        _path = path;
        LogRotation.Rotate(path, keep);
    }

    public ILogger CreateLogger(string categoryName) => new FileLogger(this, categoryName);

    public void Dispose()
    {
        lock (_gate)
        {
            _writer?.Dispose();
            _writer = null;
        }
    }

    private void Write(string category, LogLevel level, string message, Exception? exception)
    {
        var line = string.Create(CultureInfo.InvariantCulture,
            $"{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss.fff} {level,-11} {Short(category)} {message}");
        if (exception is not null)
        {
            line += Environment.NewLine + exception;
        }

        lock (_gate)
        {
            try
            {
                if (_writer is null)
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
                    _writer = new StreamWriter(
                        new FileStream(_path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite))
                    {
                        AutoFlush = true,
                    };
                }

                _writer.WriteLine(line);
            }
            catch (IOException)
            {
                // A log that cannot be written must never take the app with it
            }
        }
    }

    private static string Short(string category) =>
        category.LastIndexOf('.') is var dot && dot >= 0 ? category[(dot + 1)..] : category;

    private sealed class FileLogger(FileLoggerProvider provider, string category) : ILogger
    {
        public IDisposable? BeginScope<TState>(TState state)
            where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => logLevel >= LogLevel.Information;

        public void Log<TState>(
            LogLevel logLevel, EventId eventId, TState state, Exception? exception,
            Func<TState, Exception?, string> formatter)
        {
            if (IsEnabled(logLevel))
            {
                provider.Write(category, logLevel, formatter(state, exception), exception);
            }
        }
    }
}
