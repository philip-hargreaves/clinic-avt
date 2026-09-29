using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Guidance;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>The panes a review shows, emptied together between consultations.</summary>
public sealed class ReviewPanes(NoteViewModel note, GuidanceViewModel guidance, PageViewModel page)
{
    /// <summary>Empties the note, the guidance and the page view.</summary>
    public void Clear()
    {
        note.Reset();
        guidance.Reset();
        page.Hide();
    }
}
