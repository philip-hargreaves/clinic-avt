namespace ClinicAVT.App.Core.Features.Documents;

public interface IReviewActions
{
    Task SaveNoteAsync();

    Task SavePatientAsync();

    Task RegenerateNoteAsync();

    Task WriteNoteAnywayAsync();

    Task RegeneratePatientAsync();

    Task TranslateAsync(string language);

    Task ReflectAsync();
}
