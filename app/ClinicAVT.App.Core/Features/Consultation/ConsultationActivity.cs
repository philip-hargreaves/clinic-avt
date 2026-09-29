using CommunityToolkit.Mvvm.ComponentModel;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// Set by the recorder and read by the status bar and the record controls.
/// </summary>
public sealed partial class ConsultationActivity : ObservableObject
{
    // Nothing is being recorded, finalised or reviewed, so the next step is a recording
    [ObservableProperty]
    public partial bool Idle { get; set; } = true;

    [ObservableProperty]
    public partial bool Listening { get; set; }

    /// <summary>Microphone level, 0 to 1.</summary>
    [ObservableProperty]
    public partial double Level { get; set; }

    /// <summary>
    /// True from stop until the sealed transcript loads. The finalise tail decode is the NPU's
    /// longest stage, so the RT figure stays on screen through it.
    /// </summary>
    [ObservableProperty]
    public partial bool Decoding { get; set; }

    [ObservableProperty]
    public partial bool ShowingSample { get; set; }

    /// <summary>A recording or an import began.</summary>
    public event Action? Started;

    public void Start() => Started?.Invoke();

    public void StopListening()
    {
        Listening = false;
        Level = 0;
    }
}
