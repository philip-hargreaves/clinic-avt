namespace ClinicAVT.App.Core.Features.Appraisal;

/// <summary>What a journal card's buttons ask of the Appraisal page.</summary>
public interface IReflectionJournal
{
    Task ToggleAsync(ReflectionCard card);

    Task DeleteAsync(ReflectionCard card);
}
