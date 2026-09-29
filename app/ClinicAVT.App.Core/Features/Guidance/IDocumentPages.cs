namespace ClinicAVT.App.Core.Features.Guidance;

/// <summary>The page view beside the note, as the guidance cards use it.</summary>
public interface IDocumentPages
{
    /// <summary>Opens on the passage's page.</summary>
    Task ShowAsync(GuidanceRecommendation found);

    /// <summary>Opens the document in the PDF viewer.</summary>
    Task OpenAsync(GuidanceRecommendation found);

    /// <summary>Closes the view when the passage it shows is missing from the cards.</summary>
    void KeepOnlyFor(IEnumerable<GuidanceCard> cards);
}
