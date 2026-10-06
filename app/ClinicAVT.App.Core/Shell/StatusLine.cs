using CommunityToolkit.Mvvm.ComponentModel;
using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Shell;

/// <summary>The latest activity on the status line, and a storage fault that holds it.</summary>
public sealed partial class StatusLine : ObservableObject, IStatusLine
{
    private readonly ILogger<StatusLine> _logger;

    public StatusLine(IEngineEvents events, ConsultationActivity activity, ILogger<StatusLine> logger)
    {
        _logger = logger;
        events.Subscribe<StorageFault>(fault => ShowStorageFault(fault.Detail));
        activity.Started += ClearStorageFault;
    }

    [ObservableProperty]
    public partial string LatestActivity { get; private set; } = "";

    [ObservableProperty]
    public partial bool ActivityBusy { get; private set; }

    // The store stopped taking writes. It stays on the line until the next consultation starts
    [ObservableProperty]
    public partial string StorageFault { get; private set; } = "";

    /// <summary>Writes the line to the log file without showing it.</summary>
    public void Log(string line) => _logger.Line(line);

    public void Append(string line, bool busy = false)
    {
        _logger.Line(line);
        Show(line, busy);
    }

    /// <summary>Sets the status line without logging it, for a figure that ticks over.</summary>
    public void Show(string line, bool busy = false)
    {
        ActivityBusy = busy;
        LatestActivity = line;
    }

    private void ShowStorageFault(string detail)
    {
        StorageFault = detail.Length > 0
            ? $"Can't save to disk: {detail}. Recording continues; free some space."
            : "Can't save to disk. Recording continues; free some space.";
        _logger.Line(StorageFault);
    }

    private void ClearStorageFault() => StorageFault = "";
}
