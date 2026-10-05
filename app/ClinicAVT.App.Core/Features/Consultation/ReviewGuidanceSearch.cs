using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

public sealed class ReviewGuidanceSearch(
    IGuidanceApi engine, IStatusLine status, IUiDispatcher dispatcher, TimeProvider time,
    ReviewedSession review, SessionRecorder recorder, DocumentActions documents, GuidanceViewModel guidance)
    : IGuidanceSearch
{
    private int _documentsGeneration;

    /// <summary>
    /// How long added documents must stop changing before the note is searched again.
    /// </summary>
    public TimeSpan DocumentsSettle { get; set; } = TimeSpan.FromSeconds(3);

    // One search runs after the last document in a batch finishes. Each change bumps the generation
    // and only the latest timer fires
    public void SearchAfterDocumentsSettle()
    {
        var generation = ++_documentsGeneration;
        _ = SettleThenSearchAsync(generation);
    }

    private async Task SettleThenSearchAsync(int generation)
    {
        await Task.Delay(DocumentsSettle, time).ConfigureAwait(false);
        dispatcher.Post(() =>
        {
            if (generation == _documentsGeneration && guidance.WantsSearchAfterDocuments)
            {
                _ = SearchNoteAsync();
            }
        });
    }

    // Searches again on the note as it stands. An unsaved edit is saved first, and a
    // consultation opened meanwhile is left alone
    public async Task SearchNoteAsync()
    {
        var id = review.FinalisedSessionId;
        if (recorder.State != SessionState.Review || id is null)
        {
            return;
        }

        guidance.SearchStarted();
        await documents.SaveNoteAsync().ConfigureAwait(true);
        if (id != review.FinalisedSessionId)
        {
            return;
        }

        if (!await EngineCall.TryAsync(status, "guidance/search", () => engine.SearchGuidanceAsync(id))
            .ConfigureAwait(true))
        {
            guidance.ApplyFailed();
        }
    }

    /// <summary>
    /// The note's search came back. A result for another consultation is dropped.
    /// </summary>
    public void ApplyReady(GuidanceRecord record)
    {
        if (record.Id != review.FinalisedSessionId)
        {
            status.Log($"guidance for another session dropped: {record.Id}");
            return;
        }

        guidance.ApplyReady(record);
        if (record.StoreError is { Length: > 0 } storeError)
        {
            status.Log($"guidance not stored: {storeError}");
        }
    }

    public void ApplyFailed(GuidanceFailed failed)
    {
        if (failed.Id != review.FinalisedSessionId)
        {
            status.Log($"guidance for another session dropped: {failed.Id}");
            return;
        }

        guidance.ApplyFailed();
        status.Log($"guidance search failed: {failed.Detail}");
    }
}
