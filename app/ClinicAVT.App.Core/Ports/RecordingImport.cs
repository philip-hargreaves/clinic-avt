namespace ClinicAVT.App.Core.Ports;

/// <summary>A recording to import: the file, when the consultation began as an ISO UTC instant, and its length.</summary>
public sealed record RecordingImport(string Path, string StartedAt, double Seconds);
