namespace ClinicAVT.App.Core.Features.Documents;

/// <summary>What the review's document buttons ask of the consultation under review.</summary>
public interface IReviewActions
{
    Task SaveNoteAsync();

    Task SavePatientAsync();

    Task RegenerateNoteAsync();

    /// <summary>The clinician overrides a refusal.</summary>
    Task WriteNoteAnywayAsync();

    Task RegeneratePatientAsync();

    Task TranslateAsync(string language);

    /// <summary>Opens the appraisal reflection for the consultation on screen.</summary>
    Task ReflectAsync();
}
