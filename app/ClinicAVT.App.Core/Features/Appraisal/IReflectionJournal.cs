namespace ClinicAVT.App.Core.Features.Appraisal;

/// <summary>The Appraisal page's actions that a journal card's buttons call.</summary>
public interface IReflectionJournal
{
    Task ToggleAsync(ReflectionCard card);

    Task DeleteAsync(ReflectionCard card);
}
