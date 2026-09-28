using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Features.Demo;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// The Add consultation recording dialog. The engine reads the chosen file's length and date
/// first; the date and time can then be changed before Add.
/// </summary>
public sealed partial class ImportRecordingViewModel : ObservableObject
{
    public static readonly IReadOnlyList<string> Extensions = [".m4a", ".mp3", ".wav", ".wma", ".flac", ".aac"];

    // Checked here on the inspected length. The engine has no length rule; it refuses a note
    // with too few words
    private const double MinimumSeconds = 30;

    private const string TooShortLine = "This recording is too short to make a note from.";

    private const string FutureLine = "The date and time can't be in the future.";

    private readonly IEngineApi _engine;
    private readonly IFilePicker _picker;
    private readonly TimeProvider _clock;
    private readonly IReadOnlyList<DemoTrack> _examples;
    private bool _choosingExample;
    private int _inspection;

    public ImportRecordingViewModel(IEngineApi engine, IFilePicker picker, TimeProvider? clock = null,
        IReadOnlyList<DemoTrack>? examples = null)
    {
        _engine = engine;
        _picker = picker;
        _clock = clock ?? TimeProvider.System;
        _examples = examples ?? DemoTracks.Load();
        ExampleNames = [.. _examples.Select(e => e.Display)];
    }

    /// <summary>The bundled example consultations, which import like any recording.</summary>
    public IReadOnlyList<string> ExampleNames { get; }

    public bool ExamplesVisible => ExampleNames.Count > 0;

    /// <summary>The chosen example, -1 for none, which shows the placeholder.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(UsingExample), nameof(ShowFile), nameof(ShowDropArea), nameof(ShowWhen))]
    public partial int ExampleIndex { get; set; } = -1;

    partial void OnExampleIndexChanged(int value)
    {
        if (value >= 0 && value < _examples.Count)
        {
            _choosingExample = true;
            _ = UseFileAsync(_examples[value].Path);
            _choosingExample = false;
        }
    }

    /// <summary>An example is named in its own list and dated when added, so it shows neither.</summary>
    public bool UsingExample => ExampleIndex >= 0;

    public bool ShowFile => HasFile && !UsingExample;

    /// <summary>Stays with an example chosen, so a file can still replace it.</summary>
    public bool ShowDropArea => !ShowFile;

    public bool ShowWhen => Inspected && !UsingExample;

    public static bool IsAudio(string path) =>
        Extensions.Contains(System.IO.Path.GetExtension(path), StringComparer.OrdinalIgnoreCase);

    /// <summary>"M4A, MP3, WAV, WMA, FLAC or AAC".</summary>
    public string TypesLine { get; } =
        $"{string.Join(", ", Extensions.SkipLast(1).Select(TypeName))} or {TypeName(Extensions[^1])}";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(FileName), nameof(HasFile), nameof(NoFile), nameof(ShowFile), nameof(ShowDropArea),
        nameof(CanImport))]
    public partial string Path { get; private set; } = "";

    public string FileName => System.IO.Path.GetFileName(Path);

    public bool HasFile => Path.Length > 0;

    public bool NoFile => !HasFile;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(LengthText))]
    public partial bool Reading { get; private set; }

    /// <summary>True once the engine has read the file's length.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(LengthText), nameof(TooShort), nameof(Problem), nameof(CanImport),
        nameof(ShowWhen))]
    public partial bool Inspected { get; private set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(LengthText), nameof(TooShort), nameof(Problem), nameof(CanImport))]
    public partial double Seconds { get; private set; }

    public string LengthText => Reading ? "Reading…" : Inspected ? Words.Position(Seconds) : "";

    public bool TooShort => Inspected && Seconds < MinimumSeconds;

    /// <summary>The day the consultation took place, local.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Problem), nameof(CanImport))]
    public partial DateTimeOffset? Day { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Problem), nameof(CanImport))]
    public partial TimeSpan? Time { get; set; }

    /// <summary>The latest day the date picker offers.</summary>
    public DateTimeOffset Today
    {
        get
        {
            var now = _clock.GetLocalNow();
            return new DateTimeOffset(now.Date, now.Offset);
        }
    }

    /// <summary>What went wrong reading the file, in plain words.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Problem))]
    public partial string Error { get; private set; } = "";

    /// <summary>The dialog's warning: a failed read, a short recording or a future time.</summary>
    public string Problem =>
        Error.Length > 0 ? Error
        : TooShort ? TooShortLine
        : Inspected && InFuture ? FutureLine
        : "";

    public bool CanImport => HasFile && Inspected && !TooShort && Chosen is not null && !InFuture;

    /// <summary>What Add hands over, null until it can.</summary>
    public RecordingImport? Result => CanImport ? new RecordingImport(Path, StartedAt(), Seconds) : null;

    // The engine refuses a consultation that starts later than now
    private bool InFuture => Chosen > _clock.GetUtcNow();

    // The chosen date and time in the local zone, null until both are set
    private DateTimeOffset? Chosen
    {
        get
        {
            if (Day is not { } day || Time is not { } time)
            {
                return null;
            }

            var local = day.Date + time;
            return new DateTimeOffset(local, _clock.LocalTimeZone.GetUtcOffset(local));
        }
    }

    [RelayCommand]
    private async Task Choose()
    {
        if (await _picker.PickFileAsync(Extensions).ConfigureAwait(true) is { } path)
        {
            await UseFileAsync(path).ConfigureAwait(true);
        }
    }

    /// <summary>Takes a chosen or dropped file and reads its length and date.</summary>
    public async Task UseFileAsync(string path)
    {
        var inspection = ++_inspection;
        // A chosen or dropped file replaces an example
        if (!_choosingExample)
        {
            ExampleIndex = -1;
        }

        Path = path;
        Error = "";
        Inspected = false;
        Seconds = 0;
        if (!IsAudio(path))
        {
            Error = $"This file is not a recording ClinicAVT can import. Choose an {TypesLine} file.";
            return;
        }

        Reading = true;
        try
        {
            var info = await _engine.InspectRecordingAsync(path).ConfigureAwait(true);
            if (inspection != _inspection)
            {
                return;
            }

            Reading = false;
            Seconds = info.Seconds;
            Inspected = true;
            SetWhen(!UsingExample && DateTimeOffset.TryParse(info.RecordedAt, CultureInfo.InvariantCulture,
                    DateTimeStyles.AssumeUniversal, out var recorded)
                ? TimeZoneInfo.ConvertTime(recorded, _clock.LocalTimeZone)
                : _clock.GetLocalNow());
        }
        catch (Exception e)
        {
            if (inspection == _inspection)
            {
                Reading = false;
                Error = $"This recording could not be read: {EngineWords.Reason(e)}.";
            }
        }
    }

    private static string TypeName(string extension) => extension.TrimStart('.').ToUpperInvariant();

    // The chosen date and time as the ISO UTC instant the engine stores
    private string StartedAt() =>
        Chosen!.Value.UtcDateTime.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture);

    // To the minute, which is all the pickers show
    private void SetWhen(DateTimeOffset local)
    {
        Day = new DateTimeOffset(local.Date, local.Offset);
        Time = new TimeSpan(local.Hour, local.Minute, 0);
    }
}
