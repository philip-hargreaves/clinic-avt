using System.ComponentModel;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Core.Features.Consultation;

/// <summary>
/// The session as the engine host sees it, mirrored from the consultation view model. The
/// host is built before the view model, so this stands between them.
/// </summary>
public sealed class LiveSessionState : ISessionState
{
    private ConsultationViewModel? _session;

    public bool ConsultationInProgress { get; private set; }

    public string? ReviewedSessionId => _session?.ReviewedSessionId;

    public Task EndReviewAsync() => _session?.EndReviewAsync() ?? Task.CompletedTask;

    public string SessionPhase { get; private set; } = "";

    public void Follow(ConsultationViewModel session)
    {
        _session = session;
        Mirror(session);
        session.PropertyChanged += (_, e) => OnChanged(session, e);
    }

    private void OnChanged(ConsultationViewModel session, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(ConsultationViewModel.State) or nameof(ConsultationViewModel.Phase))
        {
            Mirror(session);
        }
    }

    private void Mirror(ConsultationViewModel session)
    {
        ConsultationInProgress = session.ConsultationInProgress;
        SessionPhase = session.SessionPhase;
    }
}
