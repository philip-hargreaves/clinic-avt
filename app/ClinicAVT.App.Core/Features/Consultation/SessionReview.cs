using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Examples;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

public sealed class SessionReview
{
    private readonly ISessionStoreApi _engine;
    private readonly IStatusLine _status;
    private readonly ReviewedSession _review;
    private readonly SessionRecorder _recorder;
    private readonly ReviewPanes _panes;
    private readonly TranscriptViewModel _transcript;
    private readonly NoteViewModel _note;
    private readonly PatientSheetViewModel _patient;
    private readonly GuidanceViewModel _guidance;
    private readonly ConsultationActivity _activity;
    private readonly DocumentActions _documents;
    private readonly ReviewGuidanceSearch _search;
    private readonly ExampleCasesViewModel _examples;
    private readonly AppPreferences _preferences;

    // Bumped by every open and close, so a slow open that was overtaken applies nothing
    private int _open;

    public SessionReview(
        ISessionStoreApi engine, IStatusLine status, ReviewedSession review, SessionRecorder recorder,
        ReviewPanes panes, TranscriptViewModel transcript, NoteViewModel note, PatientSheetViewModel patient,
        GuidanceViewModel guidance, ConsultationActivity activity, DocumentActions documents,
        ReviewGuidanceSearch search, ExampleCasesViewModel examples, AppPreferences preferences)
    {
        _engine = engine;
        _status = status;
        _review = review;
        _recorder = recorder;
        _panes = panes;
        _transcript = transcript;
        _note = note;
        _patient = patient;
        _guidance = guidance;
        _activity = activity;
        _documents = documents;
        _search = search;
        _examples = examples;
        _preferences = preferences;
        recorder.Sealed += Sealed;
    }

    /// <summary>The session the documents belong to, null before a stop or an open.</summary>
    public string? FinalisedSessionId => _review.FinalisedSessionId;

    /// <summary>
    /// True while the review shows a stored session. False for the one just recorded.
    /// </summary>
    public bool StoredOpen => _review.StoredOpen;

    // A stop sealed the session, so the documents arriving next belong to it
    private void Sealed(string id)
    {
        _review.FinalisedSessionId = id;
        _review.StartedAt = "";
        // An unkept consultation is erased on leaving, so a reflection cannot outlive it
        _note.ReflectAvailable = _preferences.KeepConsultations;
        _note.HasReflection = false;
    }

    /// <summary>
    /// Opens a stored session in review as if just sealed. It is refused while recording. Unsaved
    /// edits to the previous review are saved first.
    /// </summary>
    public async Task<bool> OpenStoredSessionAsync(string id, string startedLabel = "",
        string startedAt = "", bool hasReflection = false, bool sample = false)
    {
        if (_recorder.State is SessionState.Recording or SessionState.Finalising)
        {
            return false;
        }

        var open = ++_open;
        await AutosaveReviewAsync().ConfigureAwait(true);
        if (!await EngineCall.TryAsync(_status, "session/open", () => _engine.OpenSessionAsync(id)).ConfigureAwait(true)
            || open != _open)
        {
            return false;
        }

        _activity.ShowingSample = sample;
        _review.StoredOpen = true;
        _review.FinalisedSessionId = id;
        _review.StartedAt = startedAt;
        _review.Regenerating = false;
        _panes.Clear();
        _note.ReflectAvailable = true;  // stored, so it will still be there
        _note.HasReflection = hasReflection;
        await _recorder.LoadFinalTranscriptAsync(id).ConfigureAwait(true);

        var note = await EngineCall.TryAsync(_status, "session/note", () => _engine.StoredNoteAsync(id))
            .ConfigureAwait(true);
        var patient = await EngineCall.TryAsync(_status, "session/patient", () => _engine.StoredPatientAsync(id))
            .ConfigureAwait(true);
        if (open != _open)
        {
            return false;
        }

        var translation = patient?.Translation;
        _patient.LoadStored(patient?.Text ?? "", translation?.Text ?? "", translation?.Language ?? "");
        _note.LoadStored(
            note?.Text ?? "", note?.Style ?? "", note?.Detail ?? "",
            EditedStamp.Label(note?.GeneratedAt ?? "", note?.EditedAt ?? ""));
        _review.LoadedNote = _note.ClinicalNoteText;
        _review.LoadedPatient = _patient.PatientInfoText;
        // ISO 8601 UTC stamps compare as text. A note edited after the sheet makes it stale
        var noteEdited = note?.EditedAt ?? "";
        var sheetWritten = patient?.GeneratedAt ?? "";
        _patient.PatientStale = noteEdited.Length > 0 && sheetWritten.Length > 0
            && string.CompareOrdinal(noteEdited, sheetWritten) > 0;
        // A sheet rewritten or edited after its translation outdates it
        var sheetEdited = patient?.EditedAt ?? "";
        var sheetChanged = string.CompareOrdinal(sheetEdited, sheetWritten) > 0 ? sheetEdited : sheetWritten;
        var translated = translation?.TranslatedAt ?? "";
        _patient.TranslationStale = _patient.TranslationText.Length > 0 && translated.Length > 0
            && string.CompareOrdinal(sheetChanged, translated) > 0;
        // The guidance this note was shown, restored without a model. An empty note has none
        if (_note.ClinicalNoteText.Length > 0)
        {
            var stored = await EngineCall.TryAsync(_status, "session/guidance", () => _engine.StoredGuidanceAsync(id))
                .ConfigureAwait(true);
            if (open != _open)
            {
                return false;
            }

            // Documents were added since this note was searched, so search once the view settles
            if (_guidance.LoadStored(stored))
            {
                _search.SearchAfterDocumentsSettle();
            }
        }

        _recorder.EnterReview(panesOpen: true);
        _status.Append(startedLabel.Length > 0
            ? $"Reviewing the consultation from {startedLabel}"
            : "Reviewing a stored consultation");
        return true;
    }

    /// <summary>
    /// Leaves the review or a refusal. It saves edits, clears the panes and closes the session,
    /// which deletes a refused one.
    /// </summary>
    public async Task CloseReviewAsync()
    {
        if (_recorder.State is not (SessionState.Review or SessionState.Refused))
        {
            return;
        }

        _open++;
        await AutosaveReviewAsync().ConfigureAwait(true);
        await EngineCall.TryAsync(_status, "session/close", () => _engine.CloseSessionAsync()).ConfigureAwait(true);
        _panes.Clear();
        _transcript.Clear();
        _review.Regenerating = false;
        _review.StoredOpen = false;
        _review.FinalisedSessionId = null;
        _review.StartedAt = "";
        _review.LoadedNote = "";
        _review.LoadedPatient = "";
        _recorder.Idle();
        _status.Append("Ready");
    }

    // On leaving, edits that differ from the store are saved as the clinician's wording
    private async Task AutosaveReviewAsync()
    {
        if (_recorder.State != SessionState.Review || _review.FinalisedSessionId is null)
        {
            return;
        }

        _examples.DropExample();
        await _documents.SaveNoteAsync().ConfigureAwait(true);
        if (_patient.PatientInfoText != _review.LoadedPatient)
        {
            await _documents.SavePatientAsync().ConfigureAwait(true);
        }
    }
}
