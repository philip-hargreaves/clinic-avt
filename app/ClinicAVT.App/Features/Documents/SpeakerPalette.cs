using Microsoft.UI.Xaml;

namespace ClinicAVT.App.Features.Documents;

/// <summary>
/// The brushes are theme resources, so each element resolves them for its own theme.
/// </summary>
public static class SpeakerPalette
{
    public static Style Stripe(string speaker) => Find(Role(speaker) + "SpeakerStripe");

    public static Style Name(string speaker) => Find(Role(speaker) + "SpeakerName");

    private static string Role(string speaker) => speaker switch
    {
        "doctor" => "Doctor",
        "patient" => "Patient",
        _ => "Unknown",
    };

    private static Style Find(string key) => (Style)Application.Current.Resources[key];
}
