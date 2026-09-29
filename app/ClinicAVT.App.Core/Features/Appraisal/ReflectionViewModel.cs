using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Appraisal;

/// <summary>
/// One appraisal reflection. The engine writes the case summary, and the clinician writes the
/// three answers and ticks the guidance they referred to. It saves only when something changed.
/// </summary>
public sealed partial class ReflectionViewModel : ObservableObject, IDisposable
{
    private readonly IReflectionApi _engine;
    private readonly ISessionStoreApi _store;
    private readonly IClipboard _clipboard;
    private readonly IFilePicker _picker;
    private readonly ITextFiles _files;
    private readonly IDialogService _dialogs;
    private readonly IStatusLine _status;
    private readonly TimeProvider _time;
    private string _savedHappened = "";
    private string _savedLearned = "";
    private string _savedNext = "";
    private string _savedTitle = "";

    public ReflectionViewModel(
        IReflectionApi engine, ISessionStoreApi store, IEngineEvents events, IClipboard clipboard,
        IFilePicker picker, ITextFiles files, IDialogService dialogs, IStatusLine status,
        INoteModelLoad load, TimeProvider time)
    {
        _engine = engine;
        _store = store;
        _clipboard = clipboard;
        _picker = picker;
        _files = files;
        _dialogs = dialogs;
        _status = status;
        _time = time;
        CaseStudy = new CaseStudyViewModel(engine, events, status, load);
        Guidance = new ReflectionReferencesViewModel(store, status, SaveAsync);
        CaseStudy.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(CaseStudyViewModel.Summary))
            {
                OnPropertyChanged(nameof(Warning));
                OnPropertyChanged(nameof(HasWarning));
            }
        };
    }

    public CaseStudyViewModel CaseStudy { get; }

    public ReflectionReferencesViewModel Guidance { get; }

    /// <summary>The consultation's label. Typing here renames the consultation.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DisplayTitle))]
    public partial string Title { get; set; } = "";

    /// <summary>Dated to the month. The exact date never leaves the device.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DisplayTitle))]
    public partial string Month { get; private set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Warning))]
    [NotifyPropertyChangedFor(nameof(HasWarning))]
    public partial string Happened { get; set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Warning))]
    [NotifyPropertyChangedFor(nameof(HasWarning))]
    public partial string Learned { get; set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Warning))]
    [NotifyPropertyChangedFor(nameof(HasWarning))]
    public partial string Next { get; set; } = "";

    public string SessionId { get; private set; } = "";

    public string DisplayTitle => Title.Trim().Length > 0 ? Title.Trim() : Month;

    /// <summary>What in the text may identify the patient, empty when nothing was found.</summary>
    public string Warning =>
        IdentifierCheck.Describe(CaseStudy.Summary + "\n" + Happened + "\n" + Learned + "\n" + Next);

    public bool HasWarning => Warning.Length > 0;

    public bool Dirty =>
        Happened != _savedHappened || Learned != _savedLearned || Next != _savedNext || Guidance.Dirty;

    public string ExportText => ReflectionExport.Format(
        new ReflectionEntry(DisplayTitle, Month, CaseStudy.Summary, Happened, Learned, Next)
        {
            References = Guidance.Ticked(),
        });

    /// <summary>Loads the stored entry and asks for a summary when none exists yet.</summary>
    public async Task LoadAsync(string sessionId, string startedAt = "")
    {
        SessionId = sessionId;
        Month = Words.Month(Words.LocalTime(startedAt) ?? _time.GetLocalNow());
        var opened = await EngineCall.ReportAsync(_status, "could not open the reflection", async () =>
        {
            var got = await _engine.GetReflectionAsync(sessionId).ConfigureAwait(true);
            Title = _savedTitle = got.Label ?? "";
            CaseStudy.Load(sessionId, got.Summary?.Text ?? "");
            if (got.Answers is { } answers)
            {
                Happened = _savedHappened = Answer(answers.Happened ?? "");
                Learned = _savedLearned = Answer(answers.Learned ?? "");
                Next = _savedNext = Answer(answers.Next ?? "");
            }
            else
            {
                Happened = Learned = Next = _savedHappened = _savedLearned = _savedNext = "";
            }

            await Guidance.LoadAsync(sessionId, got.Answers?.References ?? []).ConfigureAwait(true);
        }).ConfigureAwait(true);

        if (opened && CaseStudy.Summary.Length == 0)
        {
            await CaseStudy.RequestAsync().ConfigureAwait(true);
        }
    }

    /// <summary>Writes the answers and ticks only when they changed.</summary>
    public async Task SaveAsync()
    {
        // A whitespace-only answer counts as empty, because a stray line break would hide the hint
        // and count as writing
        Happened = Answer(Happened);
        Learned = Answer(Learned);
        Next = Answer(Next);
        if (!Dirty || SessionId.Length == 0)
        {
            return;
        }

        if (await EngineCall.ReportAsync(_status, "could not save the reflection",
                () => _engine.UpdateReflectionAsync(SessionId, Happened, Learned, Next, Guidance.Ticked()))
            .ConfigureAwait(true))
        {
            _savedHappened = Happened;
            _savedLearned = Learned;
            _savedNext = Next;
            Guidance.MarkSaved();
        }
    }

    /// <summary>
    /// A retitle renames the consultation itself. A blank title keeps the old name.
    /// </summary>
    public async Task SaveTitleAsync() =>
        _savedTitle = await SessionLabel.SaveAsync(_store, _status, SessionId, Title, _savedTitle)
            .ConfigureAwait(true);

    /// <summary>Saves any changes, then unsubscribes from engine events.</summary>
    public async Task CloseAsync()
    {
        try
        {
            await SaveAsync().ConfigureAwait(true);
            await SaveTitleAsync().ConfigureAwait(true);
        }
        finally
        {
            Dispose();
        }
    }

    public void Dispose() => CaseStudy.Dispose();

    [RelayCommand]
    private Task Copy() => _clipboard.CopyAsync(_status, ExportText, "Reflection");

    [RelayCommand]
    private Task SaveAsText() =>
        ReflectionFile.SaveAsync(_dialogs, _picker, _files, _status, ExportText, DisplayTitle, Warning);

    private static string Answer(string text) => text.Trim().Length == 0 ? "" : text;
}
