using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Core.Features.Documents;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Features.Examples;

/// <summary>The written cases offered on a sample record in place of its note.</summary>
public sealed partial class ExampleCasesViewModel : ObservableObject
{
    public const string OriginalNoteTitle = "Original note";

    private readonly NoteViewModel _note;
    private readonly ReviewedSession _review;
    private readonly SessionRecorder _recorder;
    private readonly ConsultationActivity _activity;
    private readonly IStatusLine _status;
    private readonly IGuidanceSearch _search;

    public ExampleCasesViewModel(
        IExampleLibrary library, NoteViewModel note, ReviewedSession review, SessionRecorder recorder,
        ConsultationActivity activity, IStatusLine status, IGuidanceSearch search)
    {
        _note = note;
        _review = review;
        _recorder = recorder;
        _activity = activity;
        _status = status;
        _search = search;
        ExampleCases = library.LoadCases();
        activity.PropertyChanged += OnActivityChanged;
        note.Cleared += () => ExampleCaseIndex = -1;
        // Editing an example makes it the clinician's text. The overlay ends and the normal
        // save applies
        note.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(NoteViewModel.NoteEditing) && note.NoteEditing)
            {
                _review.OriginalNote = null;
            }
        };
    }

    [ObservableProperty]
    public partial bool ExampleCasesVisible { get; private set; }

    /// <summary>
    /// The picker's selection, where -1 shows its placeholder. Choosing applies the case.
    /// </summary>
    [ObservableProperty]
    public partial int ExampleCaseIndex { get; set; } = -1;

    public IReadOnlyList<ExampleCase> ExampleCases { get; }

    /// <summary>The picker's entries, the stored note first and then the cases.</summary>
    public IReadOnlyList<string> ExampleCaseTitles =>
        [OriginalNoteTitle, .. ExampleCases.Select(c => c.Title)];

    /// <summary>True while an example case shows in place of the stored note.</summary>
    public bool ExampleShown => _review.OriginalNote is not null;

    /// <summary>
    /// A written case stands in for the note of a sample record and is searched as the
    /// note is. The stored note is kept aside and written back by
    /// <see cref="RestoreOriginalNoteAsync"/> or on leaving.
    /// </summary>
    public async Task ApplyExampleCaseAsync(ExampleCase example)
    {
        if (_recorder.State != SessionState.Review || !_activity.ShowingSample)
        {
            return;
        }

        _review.OriginalNote ??= _review.LoadedNote;
        _note.ClinicalNoteText = example.Text;
        _status.Append($"Example case: {example.Title}");
        await _search.SearchNoteAsync().ConfigureAwait(true);
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
        await _search.SearchNoteAsync().ConfigureAwait(true);
    }

    /// <summary>Puts the stored note back, before a save or a leave.</summary>
    public void DropExample()
    {
        if (_review.OriginalNote is { } original)
        {
            _review.OriginalNote = null;
            _note.ClinicalNoteText = original;
        }
    }

    partial void OnExampleCaseIndexChanged(int value)
    {
        if (value == 0)
        {
            _ = RestoreOriginalNoteAsync();
        }
        else if (value > 0 && value <= ExampleCases.Count)
        {
            _ = ApplyExampleCaseAsync(ExampleCases[value - 1]);
        }
    }

    private void OnActivityChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName != nameof(ConsultationActivity.ShowingSample))
        {
            return;
        }

        ExampleCasesVisible = _activity.ShowingSample && ExampleCases.Count > 0;
        if (!_activity.ShowingSample)
        {
            ExampleCaseIndex = -1;
        }
    }
}
