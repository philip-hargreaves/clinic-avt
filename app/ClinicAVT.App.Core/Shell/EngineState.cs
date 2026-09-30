using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Shell;

/// <summary>The engine process and its connection, in the words the status line uses.</summary>
public sealed partial class EngineState : ObservableObject
{
    private readonly IStatusLine _line;

    public EngineState(IEngineHost host, IEngineEvents events, IUiDispatcher dispatcher, IStatusLine line)
    {
        _line = line;
        Ready = events.Connected;
        Status = host.Status;
        Recompute();
        host.StatusChanged += _ => dispatcher.Post(() => SetStatus(host.Status));
        events.SubscribeConnection(connected =>
        {
            Ready = connected;
            Recompute();
        });
    }

    public EngineStatus Status { get; private set; } = EngineStatus.Stopped;

    /// <summary>True while the engine is connected and answering.</summary>
    [ObservableProperty]
    public partial bool Ready { get; private set; }

    [ObservableProperty]
    public partial string EngineStateLabel { get; private set; } = "Starting";

    /// <summary>True in every transient state. The status ring spins on it.</summary>
    [ObservableProperty]
    public partial bool EngineStarting { get; private set; }

    public bool Running => Status == EngineStatus.Running;

    private void SetStatus(EngineStatus status)
    {
        Status = status;
        Recompute();
        OnPropertyChanged(nameof(Running));

        // Silent restarts stay out of the activity log. Faults go in
        if (status is EngineStatus.Faulted or EngineStatus.StoreNewer or EngineStatus.StoreTooOld)
        {
            _line.Append(EngineStateLabel);
        }
    }

    private void Recompute()
    {
        EngineStarting = Status == EngineStatus.Running && !Ready
            || Status == EngineStatus.Restarting;
        EngineStateLabel = Status switch
        {
            EngineStatus.Running when !Ready => "Starting up",
            EngineStatus.Running => "Ready",
            EngineStatus.Restarting => "Recovering",
            EngineStatus.Faulted => "Recording is unavailable - please restart the app",
            EngineStatus.StoreNewer =>
                "Your consultations were saved by a newer version of ClinicAVT. Update ClinicAVT to open them.",
            EngineStatus.StoreTooOld =>
                "Your consultations were saved by a version of ClinicAVT too old for this one to open.",
            _ => "Not running",
        };
    }
}
