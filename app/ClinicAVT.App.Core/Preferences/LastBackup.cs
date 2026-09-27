namespace ClinicAVT.App.Core.Preferences;

/// <summary>
/// A checked backup: the half-open UTC period it held as sent to the engine (empty ends are
/// open), when the engine made it and how many consultations it held.
/// </summary>
public sealed record LastBackup(string From, string To, string CreatedAt, int Consultations);
