namespace ClinicAVT.TestSupport;

/// <summary>The identity the engine gives in its hello reply.</summary>
internal static class ExpectedEngine
{
    public const string Name = "clinicavt";

    // Engine and shell share the VERSION file
    public static readonly string Version = typeof(ExpectedEngine).Assembly.GetName().Version!.ToString(3);
}
