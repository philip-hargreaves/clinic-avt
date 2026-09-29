using CommunityToolkit.Mvvm.ComponentModel;
using ClinicAVT.App.Core.Common;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// The heading over the live review. It says when the consultation started, worded as the
/// inbox lists it. The engine's label shows only in Sessions, so the heading never changes
/// underfoot.
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

    /// <summary>"Thursday 25 September, 15:09", set as a recording or an import starts.</summary>
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
