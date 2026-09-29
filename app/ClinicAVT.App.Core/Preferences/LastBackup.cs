namespace ClinicAVT.App.Core.Preferences;

/// <summary>
/// From and To are the half-open UTC period the backup held as sent to the engine. Empty ends
/// are open.
/// </summary>
public sealed record LastBackup(string From, string To, string CreatedAt, int Consultations);
