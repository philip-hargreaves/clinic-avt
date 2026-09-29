using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// Shows the start time formatted as in the inbox, because the engine's label can change while
/// the review is open.
/// </summary>
public sealed partial class ConsultationHeaderViewModel : ObservableObject
{
    private readonly IConsultation _session;
    private readonly TimeProvider _time;
    private string _id = "";

    public ConsultationHeaderViewModel(IConsultation session, TimeProvider time)
    {
        _session = session;
        _time = time;
        _session.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(IConsultation.State))
            {
                OnStateChanged();
            }
        };
        // An import never records, so its heading is the time chosen for it
        _session.ImportStarted += import =>
            Title = Words.LocalTime(import.StartedAt) is { } started ? SessionText.Heading(started) : "";
    }

    [ObservableProperty]
    public partial string Title { get; private set; } = "";

    private void OnStateChanged()
    {
        var id = _session.RecordingSessionId ?? "";
        if (_session.State == SessionState.Recording && id != _id)
        {
            _id = id;
            Title = SessionText.Heading(_time.GetLocalNow());
        }
    }
}
