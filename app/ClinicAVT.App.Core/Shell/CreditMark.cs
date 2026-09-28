namespace ClinicAVT.App.Core.Shell;

/// <summary>One partner mark. Dark falls back to the light artwork. Space evens out the gap
/// before a mark when the one ahead of it ends in thin ink, such as a trademark sign.</summary>
public sealed record CreditMark(string Name, string LightPath, string DarkPath, double Height, double Space = 0);
