namespace ClinicAVT.App.Core.Features.Guidance;

/// <summary>Searches the guidance again for the note under review.</summary>
public interface IGuidanceSearch
{
    Task SearchNoteAsync();
}
