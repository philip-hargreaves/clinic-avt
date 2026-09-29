namespace ClinicAVT.App.Core.Ports;

public interface IMicrophoneChoice
{
    /// <summary>The endpoint id session/start pins. Empty means the system default.</summary>
    string MicId { get; }
}
