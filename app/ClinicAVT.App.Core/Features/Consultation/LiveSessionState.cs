using System.ComponentModel;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// Mirrored from the consultation view model, because the host is built before the view model.
/// </summary>
public sealed class LiveSessionState : ISessionState
{
    private IConsultation? _session;

    public bool ConsultationInProgress { get; private set; }

    public string? ReviewedSessionId => _session?.ReviewedSessionId;

    public Task EndReviewAsync() => _session?.EndReviewAsync() ?? Task.CompletedTask;

    public string SessionPhase { get; private set; } = "";

    public void Follow(IConsultation session)
    {
        _session = session;
        Mirror(session);
        session.PropertyChanged += (_, e) => OnChanged(session, e);
    }

    private void OnChanged(IConsultation session, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(IConsultation.State) or nameof(IConsultation.Phase))
        {
            Mirror(session);
        }
    }

    private void Mirror(IConsultation session)
    {
        ConsultationInProgress = session.ConsultationInProgress;
        SessionPhase = session.SessionPhase;
    }
}
