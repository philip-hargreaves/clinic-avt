namespace ClinicAVT.App.Core.Shell;

/// <summary>
/// Maps mic level to the level ring. The engine's level spans 60 dB with room noise near 0.2 and
/// speech near 0.67, so the speech range is stretched over the full output.
/// </summary>
public static class LevelCurve
{
    private const double SpeechFloor = 0.25;  // below this is room noise and the ring rests
    private const double SpeechSpan = 0.55;   // loud speech reaches the top of the swing
    private const double GlowReach = 0.75;    // glow diameter beyond the disc at full level, as a fraction of it
    private const double RingReach = 0.38;    // ring swell at full level, as a fraction
    private const double GlowFloor = 0.08;    // glow alpha at silence
    private const double GlowCeiling = 0.42;
    private const double RingFloor = 0.25;    // ring alpha at silence
    private const double RingCeiling = 0.8;

    /// <summary>The level as the ring shows it, 0 to 1, eased at both ends.</summary>
    public static double Drive(double level)
    {
        var x = Math.Clamp((level - SpeechFloor) / SpeechSpan, 0.0, 1.0);
        return x * x * (3 - 2 * x);
    }

    public static double GlowScale(double level) => 1.0 + GlowReach * Drive(level);

    public static double RingScale(double level) => 1.0 + RingReach * Drive(level);

    public static double GlowAlpha(double level) => GlowFloor + (GlowCeiling - GlowFloor) * Drive(level);

    public static double RingAlpha(double level) => RingFloor + (RingCeiling - RingFloor) * Drive(level);
}
