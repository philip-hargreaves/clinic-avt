using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Core.Preferences;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Sessions;

/// <summary>
/// Past consultations. Selecting one opens it for review through the
/// consultation view model, so the shared panes show it and regenerate,
/// translate and save act on it.
/// </summary>
public sealed partial class SessionsViewModel : ObservableObject
{
    private readonly IEngineApi _engine;
    private readonly StatusBarViewModel _status;
    private readonly ConsultationViewModel _consultation;
    private readonly IDialogService _dialogs;
    private readonly AppPreferences? _preferences;

    // Set while the list moves the selection itself, on a refresh or a rename, so the
    // reselection does not reopen the session
    private bool _reselecting;

    public SessionsViewModel(
        IEngineApi engine, StatusBarViewModel status, ConsultationViewModel consultation,
        IDialogService dialogs, AppPreferences? preferences = null)
    {
        _engine = engine;
        _status = status;
        _consultation = consultation;
        _dialogs = dialogs;
        _preferences = preferences;
        // The list is on screen while a recording ends, so it follows the store
        consultation.Recorder.Sealed += id => _ = RefreshAsync();
        // The line under the title names the note's style, which a rewrite changes
        consultation.Note.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(NoteViewModel.Style) or nameof(NoteViewModel.Detail))
            {
                RefreshMeta();
            }
        };
    }

    public ObservableCollection<SessionRow> Sessions { get; } = [];

    /// <summary>The rows matching the query, grouped by day.</summary>
    public ObservableCollection<SessionGroup> Groups { get; } = [];

    /// <summary>Narrows the list to titles containing the text.</summary>
    [ObservableProperty]
    public partial string Query { get; set; } = "";

    partial void OnQueryChanged(string value) => Regroup();

    /// <summary>
    /// True when Keep consultations is off and nothing is stored, so the page explains itself
    /// instead of showing a bare empty list. Existing history always shows, and only the
    /// clinician empties it.
    /// </summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SelectHintVisible), nameof(NoneOpen))]
    public partial bool EmptyBecauseOff { get; private set; }

    [ObservableProperty]
    public partial SessionRow? Selected { get; set; }

    partial void OnSelectedChanged(SessionRow? value)
    {
        if (!_reselecting)
        {
            _ = OpenAsync(value);
        }
    }

    /// <summary>True while the selected session is open in the panes.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SelectHintVisible), nameof(NoneOpen))]
    public partial bool DetailOpen { get; private set; }

    /// <summary>The hint in the reading pane when nothing is open.</summary>
    public bool SelectHintVisible => !DetailOpen && !EmptyBecauseOff;

    /// <summary>True when consultations are kept and there are none yet.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(NoneOpen))]
    public partial bool NothingStored { get; private set; }

    /// <summary>True when there is a list and nothing from it is open.</summary>
    public bool NoneOpen => SelectHintVisible && !NothingStored;

    /// <summary>The open session's label. Editing it renames the session.</summary>
    [ObservableProperty]
    public partial string DetailTitle { get; set; } = "";

    [ObservableProperty]
    public partial string DetailMeta { get; private set; } = "";

    public NoteViewModel Note => _consultation.Note;

    /// <summary>
    /// Enters the page with the list loaded and the most recent consultation open.
    /// </summary>
    public async Task EnterAsync()
    {
        await RefreshAsync().ConfigureAwait(true);
        if (Selected is not null || Sessions.Count == 0)
        {
            return;
        }

        // The consultation just recorded is already in the shared panes. It shows as it is, so
        // going back to Consultation finds it still there
        if (Sessions.FirstOrDefault(s => s.Id == _consultation.LiveReviewId) is { } live)
        {
            Reselect(live);
            DetailOpen = true;
            DetailTitle = live.Title;
            RefreshMeta();
            return;
        }

        Selected = Sessions[0];
    }

    /// <summary>Reloads the list, keeping the open session selected if it is still there.</summary>
    public Task RefreshAsync() =>
        EngineCall.ReportAsync(_status, "could not list consultations", async () =>
        {
            var sessions = await _engine.ListSessionsAsync().ConfigureAwait(true);
            var keep = DetailOpen ? Selected?.Id : null;
            Reselect(null);
            Sessions.Clear();
            foreach (var session in sessions)
            {
                var started = session.StartedAt;
                var label = session.Label ?? "";
                var startedLabel = SessionText.Started(started);
                Sessions.Add(new SessionRow(
                    session.Id,
                    label.Length > 0 ? label : startedLabel,
                    startedLabel,
                    SessionText.Duration(session.AudioSeconds, started, session.EndedAt),
                    EditedStamp.Label(started, session.EditedAt ?? ""),
                    started,
                    session.Demo,
                    session.HasReflection));
            }

            EmptyBecauseOff = Sessions.Count == 0
                && _preferences is { KeepConsultations: false };
            NothingStored = Sessions.Count == 0 && !EmptyBecauseOff;
            Regroup();

            // The list's own selection cleared with it, so the open session is reselected
            // without reopening
            if (keep is not null && Sessions.FirstOrDefault(r => r.Id == keep) is { } again)
            {
                Reselect(again);
                DetailOpen = true;
            }
            else
            {
                DetailOpen = false;
            }
        });

    private void Reselect(SessionRow? row)
    {
        _reselecting = true;
        try
        {
            Selected = row;
        }
        finally
        {
            _reselecting = false;
        }
    }

    private void Regroup()
    {
        Groups.Clear();
        SessionGroup? group = null;
        foreach (var row in Sessions)
        {
            if (Query.Length > 0 && !row.Title.Contains(Query, StringComparison.CurrentCultureIgnoreCase))
            {
                continue;
            }

            var day = DayLabel(row.StartedAt);
            if (group is null || group.Day != day)
            {
                group = new SessionGroup(day);
                Groups.Add(group);
            }

            group.Add(row);
        }
    }

    private static string DayLabel(string startedAt)
    {
        if (Words.LocalTime(startedAt) is not { } started)
        {
            return "";
        }

        var date = started.Date;
        var today = DateTime.Today;
        if (date == today)
        {
            return "Today";
        }

        if (date == today.AddDays(-1))
        {
            return "Yesterday";
        }

        return Words.Day(started, today.Year, fullMonth: true);
    }

    private async Task OpenAsync(SessionRow? row)
    {
        if (row is null)
        {
            DetailOpen = false;
            return;
        }

        DetailOpen = await _consultation.OpenStoredSessionAsync(
                row.Id, row.Started, row.StartedAt, row.HasReflection, row.Demo)
            .ConfigureAwait(true);
        if (DetailOpen)
        {
            DetailTitle = row.Title;
            RefreshMeta();
        }
    }

    private void RefreshMeta()
    {
        if (DetailOpen && Selected is { } row)
        {
            DetailMeta = SessionText.Meta(row.Started, row.Duration, SessionText.Options(Note.Style, Note.Detail));
        }
    }

    /// <summary>Commits an edited title as the session's label.</summary>
    public async Task RenameAsync()
    {
        if (Selected is not { } row)
        {
            return;
        }

        var title = await SessionLabel.SaveAsync(_engine, _status, row.Id, DetailTitle, row.Title)
            .ConfigureAwait(true);
        if (title == row.Title)
        {
            return;
        }

        var index = Sessions.IndexOf(row);
        var renamed = row with { Title = title };
        _reselecting = true;
        try
        {
            Sessions[index] = renamed;  // replacing the item deselects it
            Regroup();
            Selected = renamed;
        }
        finally
        {
            _reselecting = false;
        }
    }

    // Deletion is crypto-erase, so it is confirmed first
    [RelayCommand]
    private async Task Delete(SessionRow row)
    {
        if (!await _dialogs.ConfirmAsync("Delete this consultation?",
                "The transcript, note and patient information are erased and cannot be recovered.",
                "Delete", "Keep").ConfigureAwait(true))
        {
            return;
        }

        await EngineCall.ReportAsync(_status, "could not delete consultation", async () =>
        {
            // Its review may be open here or, just recorded, on the Consultation page
            if (Selected?.Id == row.Id || _consultation.ReviewedSessionId == row.Id)
            {
                await _consultation.CloseReviewAsync().ConfigureAwait(true);
                DetailOpen = false;
            }

            await _engine.DeleteSessionAsync(row.Id).ConfigureAwait(true);
            _status.Append("Consultation deleted");
            await RefreshAsync().ConfigureAwait(true);
        }).ConfigureAwait(true);
    }

    /// <summary>
    /// The Back up dialog. It can remove what it backed up, so a stored review ends first and
    /// keeps its edits.
    /// </summary>
    [RelayCommand]
    private async Task BackUp()
    {
        if (!_engine.Connected || Settings.ConsultationGuard.Blocks(_consultation, _status, "backing up"))
        {
            return;
        }

        if (_consultation.ReviewingStored)
        {
            await LeaveAsync().ConfigureAwait(true);
        }

        await _dialogs.RunBackupAsync().ConfigureAwait(true);
        await RefreshAsync().ConfigureAwait(true);
    }

    /// <summary>Ends the review and saves any edits.</summary>
    public Task LeaveAsync()
    {
        Selected = null;
        DetailOpen = false;
        return _consultation.CloseReviewAsync();
    }

    /// <summary>
    /// Going to record a new consultation ends a stored review. The review of the
    /// consultation just recorded is not a stored one and stays.
    /// </summary>
    public Task CloseStoredReviewAsync() =>
        _consultation.ReviewingStored ? LeaveAsync() : Task.CompletedTask;
}
