using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Tests.TestDoubles;

public sealed class FakeSession : ISessionState
{
    public bool ConsultationInProgress { get; set; }

    public string? ReviewedSessionId { get; set; }

    public int ReviewsEnded { get; private set; }

    public Task EndReviewAsync()
    {
        ReviewsEnded++;
        ReviewedSessionId = null;
        return Task.CompletedTask;
    }

    public string SessionPhase { get; set; } = "";
}
