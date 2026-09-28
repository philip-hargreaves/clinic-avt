using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>The engine's pushes, each handed to the part of the consultation it concerns.</summary>
public sealed class NotificationRouter
{
    private readonly SessionRecorder _recorder;
    private readonly SessionReview _review;
    private readonly ConsultationReadiness _readiness;
    private readonly NoteViewModel _note;
    private readonly GuidanceViewModel _guidance;
    private readonly StatusBarViewModel _status;
    private readonly Metrics.PerformanceCollector? _metrics;

    public NotificationRouter(
        SessionRecorder recorder, SessionReview review, ConsultationReadiness readiness, NoteViewModel note,
        GuidanceViewModel guidance, StatusBarViewModel status, Metrics.PerformanceCollector? metrics)
    {
        _recorder = recorder;
        _review = review;
        _readiness = readiness;
        _note = note;
        _guidance = guidance;
        _status = status;
        _metrics = metrics;
    }

    private SessionState State => _recorder.State;

    // Note events count from a stop, and in review because a regenerate streams there
    private bool NoteExpected => State is SessionState.Finalising or SessionState.Review;

    // A rewrite's timings stay out of the per-session metrics
    private Metrics.PerformanceCollector? SessionMetrics => _review.Regenerating ? null : _metrics;

    public void Route(EngineNotification notification)
    {
        if (notification is NoteModelState { State: "ready" } resident)
        {
            _metrics?.NoteModel(resident.Name, resident.Tier, resident.Seconds);
        }

        switch (notification)
        {
            case NoteModelState model:
                _readiness.NoteModelChanged(model);
                break;
            case SessionProgress progress:
                _recorder.AdvancePhase(progress.Stage);
                break;
            case ImportProgress progress:
                _recorder.OnImportProgress(progress);
                break;
            case NotePartial chunk when NoteExpected:
                // Writing is claimed only once tokens stream
                if (_note.ClinicalNoteText.Length == 0)
                {
                    _status.Append("Writing clinical note", busy: true);
                    _recorder.NoteStreaming();
                }

                _note.ClinicalNoteText = chunk.Text;
                SessionMetrics?.NotePartial(chunk.TokensPerSecond);
                break;
            case NoteReady ready when NoteExpected:
                if (ready.Text is { } noteText)
                {
                    _note.ClinicalNoteText = noteText;
                }

                _note.Apply(NotePipelineEvent.NoteReady);
                _review.LoadedNote = _note.ClinicalNoteText;
                _recorder.EnterReview();
                _guidance.NoteReady();
                SessionMetrics?.NoteReady(ready.TokensPerSecond);
                break;
            // A recording too short or not clinical leaves nothing to review, so the record
            // region says why and offers the override. A refusal while already reviewing
            // shows in the note pane instead
            case NoteRefused refused when NoteExpected:
                _note.RefusalReason = refused.Reason;
                _note.WriteAnywayAvailable = refused.Overridable;
                _note.Apply(NotePipelineEvent.NoteRefused);
                _guidance.NoteFailed();
                _recorder.Refuse();
                _status.Append("No note - too short or not enough clinical information");
                _ = SessionMetrics?.SessionFinishedAsync("refused: " + _note.RefusalReason, 0);
                break;
            // The transcript is still usable, so review proceeds without a note
            case NoteFailed failed when NoteExpected:
                _note.Apply(NotePipelineEvent.NoteFailed);
                _guidance.NoteFailed();
                _recorder.EnterReview();
                _status.Append("Clinical note failed");
                _ = SessionMetrics?.SessionFinishedAsync(failed.Detail, 0);
                _review.Regenerating = false;
                break;
            case PatientPartial chunk:
                if (_note.PatientInfoText.Length == 0)
                {
                    _status.Append("Writing patient information", busy: true);
                }

                _note.PatientInfoText = chunk.Text;
                SessionMetrics?.PatientPartial(chunk.TokensPerSecond);
                break;
            case PatientReady ready:
                if (ready.Text is { } patientText)
                {
                    _note.PatientInfoText = patientText;
                }

                _note.Apply(NotePipelineEvent.PatientInfoReady);
                _review.LoadedPatient = _note.PatientInfoText;
                _note.PatientStale = false;
                // A rewritten sheet no longer matches a translation of the old one
                if (_note.TranslationVisible)
                {
                    _note.TranslationStale = true;
                }

                _status.Append("Ready for review");
                _ = SessionMetrics?.SessionFinishedAsync(null, _note.ClinicalNoteText.Length,
                    patientTokensPerSecond: ready.TokensPerSecond);
                _review.Regenerating = false;
                break;
            case PatientFailed:
                _note.Apply(NotePipelineEvent.PatientInfoFailed);
                _status.Append("Patient information failed");
                _ = SessionMetrics?.SessionFinishedAsync(null, _note.ClinicalNoteText.Length, "failed");
                _review.Regenerating = false;
                break;
            case TranslationPartial chunk:
                _note.TranslationText = chunk.Text;
                break;
            case TranslationReady ready:
                _note.TranslationText = ready.Text;
                _note.TranslationLanguage = ready.Language;
                _note.TranslationRunning = false;
                _status.Append($"Translated to {_note.TranslationLanguage}");
                break;
            case TranslationFailed:
                _note.TranslationRunning = false;
                _status.Append("Translation failed");
                break;
            case GuidanceModelChanged:
                _ = _readiness.LoadGuidanceReadinessAsync();
                break;
            case GuidanceReady ready:
                _review.ApplyGuidance(ready.Record);
                break;
            case GuidanceFailed failed:
                _review.ApplyGuidanceFailed(failed);
                break;
            case GuidanceDocumentsChanged:
                _guidance.DocumentsChanged();
                _review.SearchAfterDocumentsSettle();
                break;
            case AudioLevel level:
                _recorder.OnAudioLevel(level);
                break;
            case SessionInterrupted interrupted
                when State is SessionState.Recording or SessionState.Finalising:
                _recorder.Interrupt(interrupted.Detail);
                break;
        }
    }
}
