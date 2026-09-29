using System.Collections.ObjectModel;
using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Appraisal;

/// <summary>
/// The guidance a reflection refers to: the consultation's guidance, ticked, and lines the
/// clinician added. Every change is saved through the owning reflection.
/// </summary>
public sealed partial class ReflectionReferencesViewModel : ObservableObject
{
    /// <summary>Keys of the references the clinician typed start with this.</summary>
    public const string TypedKey = "typed";

    private readonly ISessionStoreApi _store;
    private readonly IStatusLine _status;
    private readonly Func<Task> _save;
    private List<string> _savedReferences = [];
    private List<string> _savedAdded = [];

    public ReflectionReferencesViewModel(ISessionStoreApi store, IStatusLine status, Func<Task> save)
    {
        _store = store;
        _status = status;
        _save = save;
    }

    /// <summary>
    /// Guidance used in the consultation, plus any ticked items the review does not show.
    /// Ticking saves.
    /// </summary>
    public ObservableCollection<ReflectionReferenceRow> References { get; } = [];

    public bool HasReferences => References.Count > 0;

    /// <summary>Guidance the clinician added themselves, one line each, always included.</summary>
    public ObservableCollection<AddedGuidanceRow> Added { get; } = [];

    public bool HasAdded => Added.Count > 0;

    /// <summary>The line being typed. Enter turns it into an entry.</summary>
    [ObservableProperty]
    public partial string Draft { get; set; } = "";

    public bool Dirty => !TickedIds().SequenceEqual(_savedReferences) || !AddedTitles().SequenceEqual(_savedAdded);

    // One row per review card, in card order. A ticked guideline missing from the review
    // gets a row from the stored reference
    public async Task LoadAsync(string sessionId, IReadOnlyList<ReflectionReference> ticked)
    {
        var guidance = await EngineCall.ReportAsync(_status, "could not read the consultation's guidance",
            () => _store.StoredGuidanceAsync(sessionId)).ConfigureAwait(true);
        Added.Clear();
        foreach (var typed in ticked.Where(r => IsTyped(r.Key)))
        {
            Added.Add(Row(typed));
        }

        _savedAdded = AddedTitles();
        OnPropertyChanged(nameof(HasAdded));
        ticked = ticked.Where(r => !IsTyped(r.Key)).ToList();
        var tickedKeys = ticked.Select(r => r.Key).ToHashSet(StringComparer.Ordinal);
        foreach (var row in References)
        {
            row.PropertyChanged -= OnReferenceChanged;
        }

        References.Clear();
        if (guidance is not null)
        {
            foreach (var card in GuidanceCard.Group(GuidanceRecommendation.ReadAll(guidance, true)))
            {
                var row = ReflectionReferenceRow.From(card, false);
                row.Ticked = tickedKeys.Contains(row.Key);
                References.Add(row);
            }
        }

        var listed = References.Select(r => r.Key).ToHashSet(StringComparer.Ordinal);
        foreach (var reference in ticked.Where(r => !listed.Contains(r.Key)))
        {
            References.Add(new ReflectionReferenceRow(reference, true));
        }

        _savedReferences = TickedIds();
        foreach (var row in References)
        {
            row.PropertyChanged += OnReferenceChanged;
        }

        OnPropertyChanged(nameof(HasReferences));
    }

    /// <summary>Records what the store now holds, after a save.</summary>
    public void MarkSaved()
    {
        _savedReferences = TickedIds();
        _savedAdded = AddedTitles();
    }

    // The ticked rows, then what the clinician added, each a reference of its own
    public List<ReflectionReference> Ticked() =>
        [.. References.Where(r => r.Ticked).Select(r => r.Stored), .. Added.Select(r => r.Stored)];

    // Saves on tick so the views don't have to
    private void OnReferenceChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(ReflectionReferenceRow.Ticked))
        {
            _ = _save();
        }
    }

    private AddedGuidanceRow Row(ReflectionReference stored)
    {
        AddedGuidanceRow? row = null;
        row = new AddedGuidanceRow(stored, new AsyncRelayCommand(() => RemoveAddedAsync(row!)));
        return row;
    }

    private static bool IsTyped(string key) => key.StartsWith(TypedKey, StringComparison.Ordinal);

    private List<string> TickedIds() =>
        References.Where(r => r.Ticked).Select(r => r.Key).ToList();

    private List<string> AddedTitles() => Added.Select(r => r.Title).ToList();

    /// <summary>The typed line becomes an entry and is saved. A blank line adds nothing.</summary>
    [RelayCommand]
    private async Task AddDraft()
    {
        var text = Draft.Trim();
        Draft = "";
        if (text.Length == 0)
        {
            return;
        }

        Added.Add(Row(new ReflectionReference($"{TypedKey}:{Guid.NewGuid():N}", "", text)));
        OnPropertyChanged(nameof(HasAdded));
        await _save().ConfigureAwait(true);
    }

    private async Task RemoveAddedAsync(AddedGuidanceRow row)
    {
        if (Added.Remove(row))
        {
            OnPropertyChanged(nameof(HasAdded));
            await _save().ConfigureAwait(true);
        }
    }
}
