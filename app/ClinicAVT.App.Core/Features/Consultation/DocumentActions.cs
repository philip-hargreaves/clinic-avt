using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Appraisal;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>The saves, rewrites, translation and reflection of the documents under review.</summary>
public sealed class DocumentActions(
    INoteApi engine, IStatusLine status, ReviewedSession review, SessionRecorder recorder,
    NoteViewModel note, PatientSheetViewModel patient, GuidanceViewModel guidance,
    IDialogService dialogs, Func<ReflectionViewModel> reflections, TimeProvider time)
    : IReviewActions
{
    public async Task TranslateAsync(string language)
    {
        if (review.FinalisedSessionId is not { } id)
        {
            return;
        }

        patient.TranslationText = "";
        patient.TranslationStale = false;
        await EngineCall.TryAsync(status, "patient/translate", () => engine.TranslatePatientAsync(id, language))
            .ConfigureAwait(true);
    }

    // Opening the sheet creates the entry, so the button reads Open from here on
    public async Task ReflectAsync()
    {
        if (review.FinalisedSessionId is not { } id)
        {
            return;
        }

        // The dialog disposes the view model as it closes. The using covers a load that fails first
        using (var reflection = reflections())
        {
            await reflection.LoadAsync(id, review.StartedAt).ConfigureAwait(true);
            await dialogs.ShowAsync(reflection).ConfigureAwait(true);
        }

        note.HasReflection = true;
    }

    // Unsaved note edits are saved first, so the rewrite reads what is on screen
    public async Task RegeneratePatientAsync()
    {
        if (recorder.State != SessionState.Review)
        {
            return;
        }

        await SaveNoteAsync().ConfigureAwait(true);
        var accepted = await EngineCall.TryAsync(status, "patient/regenerate", () => engine.RegeneratePatientAsync())
            .ConfigureAwait(true);
        if (accepted)
        {
            review.Regenerating = true;
            patient.PatientInfoText = "";
            status.Append("Rewriting patient information", busy: true);
        }
    }

    public async Task RegenerateNoteAsync()
    {
        if (recorder.State != SessionState.Review)
        {
            return;
        }

        var accepted = await EngineCall.TryAsync(status, "note/regenerate",
            () => engine.RegenerateNoteAsync(note.Style, note.Detail)).ConfigureAwait(true);
        if (accepted)
        {
            review.OriginalNote = null;  // the rewrite replaces whatever showed
            NoteRewriteStarted();
        }
    }

    /// <summary>
    /// The clinician confirms it was a consultation, so the note lane runs with the refusal off.
    /// </summary>
    public async Task WriteNoteAnywayAsync()
    {
        if (recorder.State is not (SessionState.Review or SessionState.Refused))
        {
            return;
        }

        var accepted = await EngineCall.TryAsync(status, "note/regenerate",
            () => engine.RegenerateNoteAsync(note.Style, note.Detail, confirmed: true)).ConfigureAwait(true);
        if (accepted)
        {
            NoteRewriteStarted();
            recorder.EnterReview();  // the clinician insisted, so show the transcript and note
        }
    }

    private void NoteRewriteStarted()
    {
        review.Regenerating = true;
        note.BeginRegenerate();
        guidance.NoteStarted();
    }

    // An unchanged note is not saved, because every write counts as an edit in the store and
    // would mark the sheet and the guidance stale for nothing. A consultation opened during
    // the save keeps its own stamps
    public async Task SaveNoteAsync()
    {
        var id = review.FinalisedSessionId;
        var text = note.ClinicalNoteText;
        if (id is null || text == review.LoadedNote)
        {
            return;
        }

        var saved = await EngineCall.TryAsync(status, "note/update", () => engine.UpdateNoteAsync(id, text))
            .ConfigureAwait(true);
        if (saved && id == review.FinalisedSessionId)
        {
            review.LoadedNote = text;
            note.EditedStamp = EditedStamp.Now(time);
            patient.PatientStale = patient.PatientInfoText.Length > 0;
            guidance.MarkStale();
            status.Append("Note saved");
        }
    }

    public async Task SavePatientAsync()
    {
        if (review.FinalisedSessionId is not { } id)
        {
            return;
        }

        var changed = patient.PatientInfoText != review.LoadedPatient;
        var saved = await EngineCall.TryAsync(status, "patient/update",
            () => engine.UpdatePatientAsync(id, patient.PatientInfoText)).ConfigureAwait(true);
        if (saved)
        {
            // A changed sheet outdates its translation, even one still running
            if (changed && patient.TranslationVisible)
            {
                patient.TranslationStale = true;
            }

            review.LoadedPatient = patient.PatientInfoText;
            status.Append("Patient information saved");
        }
    }
}
