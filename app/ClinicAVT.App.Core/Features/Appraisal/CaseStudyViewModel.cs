using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Features.Appraisal;

/// <summary>A reflection's case summary, written by the engine and correctable by the clinician.</summary>
public sealed partial class CaseStudyViewModel : ObservableObject, IDisposable
{
    private readonly IReflectionApi _engine;
    private readonly IStatusLine _status;
    private readonly INoteModelLoad _load;
    private readonly IDisposable _notifications;
    private readonly IDisposable _connection;
    private string _sessionId = "";
    private string _savedSummary = "";

    public CaseStudyViewModel(IReflectionApi engine, IEngineEvents events, IStatusLine status, INoteModelLoad load)
    {
        _engine = engine;
        _status = status;
        _load = load;
        _notifications = events.Subscribe<EngineNotification>(HandleNotification);
        _connection = events.SubscribeConnection(HandleConnected);
        _load.PropertyChanged += OnLoadChanged;
    }

    [ObservableProperty]
    public partial string Summary { get; set; } = "";

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(RewriteSummaryCommand))]
    [NotifyPropertyChangedFor(nameof(SummaryPlaceholder))]
    public partial bool SummaryPending { get; private set; }

    /// <summary>Shown only while a summary is pending, with the load time if the note model
    /// is still loading.</summary>
    public string SummaryPlaceholder =>
        !SummaryPending ? ""
        : _load.ModelLoading ? $"Waiting for the note model to load · {_load.ModelLoadElapsed}"
        : "Writing the case study";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasSummaryProblem))]
    public partial string SummaryProblem { get; private set; } = "";

    public bool HasSummaryProblem => SummaryProblem.Length > 0;

    /// <summary>The stored summary, empty when there is none yet.</summary>
    public void Load(string sessionId, string summary)
    {
        _sessionId = sessionId;
        Summary = _savedSummary = summary;
    }

    /// <summary>Saves the clinician's correction to the summary as their own wording.</summary>
    public async Task SaveAsync()
    {
        if (_sessionId.Length == 0 || Summary == _savedSummary)
        {
            return;
        }

        if (await EngineCall.ReportAsync(_status, "could not save the summary",
                () => _engine.UpdateReflectionSummaryAsync(_sessionId, Summary)).ConfigureAwait(true))
        {
            _savedSummary = Summary;
        }
    }

    public void Dispose()
    {
        _notifications.Dispose();
        _connection.Dispose();
        _load.PropertyChanged -= OnLoadChanged;
    }

    [RelayCommand(CanExecute = nameof(CanRewriteSummary))]
    private Task RewriteSummary() => RequestAsync();

    private bool CanRewriteSummary() => !SummaryPending && _sessionId.Length > 0;

    /// <summary>Asks the engine to write the summary. It arrives as a notification.</summary>
    public async Task RequestAsync()
    {
        SummaryPending = true;
        SummaryProblem = "";
        try
        {
            await _engine.SummariseReflectionAsync(_sessionId).ConfigureAwait(true);
        }
        catch (Exception e)
        {
            _status.Log($"reflection/summary failed: {e.Message}");
            SummaryPending = false;
            SummaryProblem = $"No summary: {EngineWords.Reason(e)}. Rewrite to try again";
        }
    }

    private void OnLoadChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (SummaryPending && e.PropertyName is nameof(INoteModelLoad.ModelLoadLine))
        {
            OnPropertyChanged(nameof(SummaryPlaceholder));
        }
    }

    // An engine restart loses any summary in progress
    private void HandleConnected(bool connected)
    {
        if (!connected && SummaryPending)
        {
            SummaryPending = false;
            SummaryProblem = "No summary: ClinicAVT restarted. Rewrite to try again";
        }
    }

    private void HandleNotification(EngineNotification notification)
    {
        switch (notification)
        {
            case ReflectionSummaryReady ready when ready.Id == _sessionId:
                Summary = _savedSummary = ready.Text;
                SummaryPending = false;
                SummaryProblem = "";
                break;
            case ReflectionSummaryFailed failed when failed.Id == _sessionId:
                SummaryPending = false;
                SummaryProblem = $"No summary: {failed.Detail}";
                break;
            default:
                break;
        }
    }
}
