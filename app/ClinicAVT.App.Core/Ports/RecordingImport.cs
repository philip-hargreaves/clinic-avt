namespace ClinicAVT.App.Core.Ports;

/// <summary>StartedAt is when the consultation began, as an ISO UTC instant.</summary>
public sealed record RecordingImport(string Path, string StartedAt, double Seconds);
