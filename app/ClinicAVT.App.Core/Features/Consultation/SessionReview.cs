using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Demo;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// The consultation under review, just sealed or reopened from the store. It owns the
/// documents, their saves and rewrites, the translation, the reflection and the guidance
/// search.
/// </summary>
public sealed class SessionReview
{
    private readonly IEngineApi _engine;
    private readonly IUiDispatcher _dispatcher;
    private readonly StatusBarViewModel _status;
    private readonly NoteViewModel _note;
    private readonly GuidanceViewModel _guidance;
    private readonly TranscriptViewModel _transcript;
    private readonly IDialogService _dialogs;
    private readonly SessionRecorder _recorder;
    private readonly AppPreferences? _preferences;

    private string _finalisedStartedAt = "";

    // The stored note while an example case stands in for it, so it can come back
    private string? _originalNote;

    // Bumped by every open and close, so a slow open that was overtaken applies nothing
    private int _open;
    private int _documentsGeneration;

    public SessionReview(
        IEngineApi engine, IUiDispatcher dispatcher, StatusBarViewModel status, NoteViewModel note,
        GuidanceViewModel guidance, TranscriptViewModel transcript, IDialogService dialogs,
        SessionRecorder recorder, AppPreferences? preferences)
    {
        _engine = engine;
        _dispatcher = dispatcher;
        _status = status;
        _note = note;
        _guidance = guidance;
        _transcript = transcript;
        _dialogs = dialogs;
        _recorder = recorder;
        _preferences = preferences;
        recorder.Sealed += Sealed;
        // Editing an example makes it the clinician's text. The overlay ends and the normal
        // save applies
        note.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(NoteViewModel.NoteEditing) && note.NoteEditing)
            {
                _originalNote = null;
            }
        };
    }

    /// <summary>True while an example case shows in place of the stored note.</summary>
    public bool ExampleShown => _originalNote is not null;

    /// <summary>The session the documents belong to, null before a stop or an open.</summary>
    public string? FinalisedSessionId { get; private set; }

    /// <summary>What the store holds, for autosaving in-place edits on leave.</summary>
    public string LoadedNote { get; set; } = "";

    public string LoadedPatient { get; set; } = "";

    /// <summary>
    /// True from a regenerate request until its pipeline settles. It keeps the rewrite out of
    /// the per-session metrics.
    /// </summary>
    public bool Regenerating { get; set; }

    /// <summary>
    /// How long added documents must stop changing before the note is searched again.
    /// </summary>
    public TimeSpan DocumentsSettle { get; set; } = TimeSpan.FromSeconds(3);

    /// <summary>
    /// True while the review shows a stored session. False for the one just recorded.
    /// </summary>
    public bool StoredOpen { get; private set; }

    // A stop sealed the session, so the documents arriving next belong to it
    private void Sealed(string id)
    {
        FinalisedSessionId = id;
        _finalisedStartedAt = "";
        // An unkept consultation is erased on leaving, so a reflection cannot outlive it
        _note.ReflectAvailable = _preferences?.KeepConsultations != false;
        _note.HasReflection = false;
    }

    /// <summary>
    /// A written case stands in for the note of a demo record and is searched as the
    /// note is. The stored note is kept aside and written back by
    /// <see cref="RestoreOriginalNoteAsync"/> or on leaving.
    /// </summary>
    public async Task ApplyExampleCaseAsync(DemoCase example)
    {
        if (_recorder.State != SessionState.Review || !_recorder.DemoRecord)
        {
            return;
        }

        _originalNote ??= LoadedNote;
        _note.ClinicalNoteText = example.Text;
        _status.Append($"Example case: {example.Title}");
        await SearchGuidanceAsync().ConfigureAwait(true);
    }

    /// <summary>Puts the stored note back in place of an example and searches again.</summary>
    public async Task RestoreOriginalNoteAsync()
    {
        if (!ExampleShown || _recorder.State != SessionState.Review)
        {
            return;
        }

        DropExample();
        _status.Append("Original note");
        await SearchGuidanceAsync().ConfigureAwait(true);
    }

    private void DropExample()
    {
        if (_originalNote is { } original)
        {
            _originalNote = null;
            _note.ClinicalNoteText = original;
        }
    }

    // A batch of documents finishing one after another searches once, after the last lands.
    // Every change starts a new generation and only the latest one's timer acts
    public void SearchAfterDocumentsSettle()
    {
        var generation = ++_documentsGeneration;
        _ = Task.Run(async () =>
        {
            await Task.Delay(DocumentsSettle).ConfigureAwait(false);
            _dispatcher.Post(() =>
            {
                if (generation == _documentsGeneration && _guidance.WantsSearchAfterDocuments)
                {
                    _ = SearchGuidanceAsync();
                }
            });
        });
    }

    // Searches again on the note as it stands. An unsaved edit is saved first, and a
    // consultation opened meanwhile is left alone
    public async Task SearchGuidanceAsync()
    {
        var id = FinalisedSessionId;
        if (_recorder.State != SessionState.Review || id is null)
        {
            return;
        }

        _guidance.SearchStarted();
        await SaveNoteAsync().ConfigureAwait(true);
        if (id != FinalisedSessionId)
        {
            return;
        }

        if (!await EngineCall.TryAsync(_status, "guidance/search", () => _engine.SearchGuidanceAsync(id))
            .ConfigureAwait(true))
        {
            _guidance.ApplyFailed();
        }
    }

    public async Task SearchGuidanceAsync(string text)
    {
        if (!await EngineCall.TryAsync(_status, "guidance/search", () => _engine.SearchGuidanceAsync(text, 3))
            .ConfigureAwait(true))
        {
            _guidance.ApplyQueryFailed();
        }
    }

    // Results are keyed to the consultation on screen. A typed query has no id.
    // A search replaced by a newer one says so and changes nothing
    public void ApplyGuidance(GuidanceRecord record)
    {
        if (record.Detail == "superseded")
        {
            return;
        }

        if (record.Id is null)
        {
            _guidance.ApplyQueryReady(record);
            return;
        }

        if (record.Id != FinalisedSessionId)
        {
            _status.Log($"guidance for another session dropped: {record.Id}");
            return;
        }

        _guidance.ApplyReady(record);
        if (record.StoreError is { Length: > 0 } storeError)
        {
            _status.Log($"guidance not stored: {storeError}");
        }
    }

    public void ApplyGuidanceFailed(GuidanceFailed failed)
    {
        if (failed.Detail == "superseded")
        {
            return;
        }

        if (failed.Id is not null && failed.Id != FinalisedSessionId)
        {
            _status.Log($"guidance for another session dropped: {failed.Id}");
            return;
        }

        if (failed.Id is null)
        {
            _guidance.ApplyQueryFailed();
        }
        else
        {
            _guidance.ApplyFailed();
        }

        _status.Log($"guidance search failed: {failed.Detail}");
    }

    public async Task TranslateAsync(string language)
    {
        if (FinalisedSessionId is not { } id)
        {
            return;
        }

        _note.TranslationText = "";
        _note.TranslationStale = false;
        await EngineCall.TryAsync(_status, "patient/translate", () => _engine.TranslatePatientAsync(id, language))
            .ConfigureAwait(true);
    }

    // Opening the sheet creates the entry, so the button reads Open from here on
    public async Task ReflectAsync()
    {
        if (FinalisedSessionId is not { } id)
        {
            return;
        }

        await _dialogs.ShowReflectionAsync(id, _finalisedStartedAt).ConfigureAwait(true);
        _note.HasReflection = true;
    }

    // Unsaved note edits are saved first, so the rewrite reads what is on screen
    public async Task RegeneratePatientAsync()
    {
        if (_recorder.State != SessionState.Review)
        {
            return;
        }

        await SaveNoteAsync().ConfigureAwait(true);
        var accepted = await EngineCall.TryAsync(_status, "patient/regenerate", () => _engine.RegeneratePatientAsync())
            .ConfigureAwait(true);
        if (accepted)
        {
            Regenerating = true;
            _note.PatientInfoText = "";
            _status.Append("Rewriting patient sheet", busy: true);
        }
    }

    public async Task RegenerateNoteAsync()
    {
        if (_recorder.State != SessionState.Review)
        {
            return;
        }

        var accepted = await EngineCall.TryAsync(_status, "note/regenerate",
            () => _engine.RegenerateNoteAsync(_note.Style, _note.Detail)).ConfigureAwait(true);
        if (accepted)
        {
            _originalNote = null;  // the rewrite replaces whatever showed
            NoteRewriteStarted();
        }
    }

    /// <summary>
    /// The clinician confirms it was a consultation, so the note lane runs with the refusal off.
    /// </summary>
    public async Task WriteNoteAnywayAsync()
    {
        if (_recorder.State is not (SessionState.Review or SessionState.Refused))
        {
            return;
        }

        var accepted = await EngineCall.TryAsync(_status, "note/regenerate",
            () => _engine.RegenerateNoteAsync(_note.Style, _note.Detail, confirmed: true)).ConfigureAwait(true);
        if (accepted)
        {
            NoteRewriteStarted();
            _recorder.EnterReview();  // the clinician insisted, so show the transcript and note
        }
    }

    private void NoteRewriteStarted()
    {
        Regenerating = true;
        _note.BeginRegenerate();
        _guidance.NoteStarted();
    }

    // An unchanged note is not saved, because every write counts as an edit in the store and
    // would mark the sheet and the guidance stale for nothing. A consultation opened during
    // the save keeps its own stamps
    public async Task SaveNoteAsync()
    {
        var id = FinalisedSessionId;
        var text = _note.ClinicalNoteText;
        if (id is null || text == LoadedNote)
        {
            return;
        }

        var saved = await EngineCall.TryAsync(_status, "note/update", () => _engine.UpdateNoteAsync(id, text))
            .ConfigureAwait(true);
        if (saved && id == FinalisedSessionId)
        {
            LoadedNote = text;
            _note.EditedStamp = EditedStamp.Now();
            _note.PatientStale = _note.PatientInfoText.Length > 0;
            _guidance.MarkStale();
            _status.Append("Note saved");
        }
    }

    public async Task SavePatientAsync()
    {
        if (FinalisedSessionId is not { } id)
        {
            return;
        }

        var changed = _note.PatientInfoText != LoadedPatient;
        var saved = await EngineCall.TryAsync(_status, "patient/update",
            () => _engine.UpdatePatientAsync(id, _note.PatientInfoText)).ConfigureAwait(true);
        if (saved)
        {
            // A changed sheet outdates its translation, even one still running
            if (changed && _note.TranslationVisible)
            {
                _note.TranslationStale = true;
            }

            LoadedPatient = _note.PatientInfoText;
            _status.Append("Patient note saved");
        }
    }

    /// <summary>
    /// A stored session becomes the review as if it had just been sealed. The same panes show,
    /// and regenerate, translate and save act on it. It is refused while recording. Unsaved
    /// edits to the previous review are saved first.
    /// </summary>
    public async Task<bool> OpenStoredSessionAsync(string id, string startedLabel = "",
        string startedAt = "", bool hasReflection = false, bool demo = false)
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

        _recorder.ShowDemo(demo);
        StoredOpen = true;
        FinalisedSessionId = id;
        _finalisedStartedAt = startedAt;
        Regenerating = false;
        _recorder.ClearPanes();
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
        _note.LoadStored(
            note?.Text ?? "", patient?.Text ?? "", translation?.Text ?? "",
            note?.Style ?? "", note?.Detail ?? "",
            EditedStamp.Label(note?.GeneratedAt ?? "", note?.EditedAt ?? ""),
            translation?.Language ?? "");
        LoadedNote = _note.ClinicalNoteText;
        LoadedPatient = _note.PatientInfoText;
        // ISO 8601 UTC stamps compare as text. A note edited after the sheet makes it stale
        var noteEdited = note?.EditedAt ?? "";
        var sheetWritten = patient?.GeneratedAt ?? "";
        _note.PatientStale = noteEdited.Length > 0 && sheetWritten.Length > 0
            && string.CompareOrdinal(noteEdited, sheetWritten) > 0;
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
                SearchAfterDocumentsSettle();
            }
        }

        _recorder.EnterReview(panesOpen: true);
        _status.Append(startedLabel.Length > 0
            ? $"Reviewing the consultation from {startedLabel}"
            : "Reviewing a stored consultation");
        return true;
    }

    /// <summary>
    /// Leaves the review or a refusal. Edits are saved and the panes clear. Telling the engine
    /// deletes a refused session.
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
        _recorder.ClearPanes();
        _transcript.Clear();
        Regenerating = false;
        StoredOpen = false;
        FinalisedSessionId = null;
        _finalisedStartedAt = "";
        LoadedNote = "";
        LoadedPatient = "";
        _recorder.Idle();
        _status.Append("Ready");
    }

    // In-place edits persist without a click. Whatever differs from the store when the
    // clinician moves on is saved as their wording
    private async Task AutosaveReviewAsync()
    {
        if (_recorder.State != SessionState.Review || FinalisedSessionId is null)
        {
            return;
        }

        DropExample();
        await SaveNoteAsync().ConfigureAwait(true);
        if (_note.PatientInfoText != LoadedPatient)
        {
            await SavePatientAsync().ConfigureAwait(true);
        }
    }
}
