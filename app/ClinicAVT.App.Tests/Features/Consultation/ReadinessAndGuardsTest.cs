using ClinicAVT.App.Core.Features.Consultation;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Features.Consultation;

public class ReadinessAndGuardsTest
{
    [Fact]
    public void AStrayNoteHostAndAFailedLanguageListAreBothReportedAtStart()
    {
        var engine = new FakeEngineClient(autoNotify: false)
        {
            StrayNoteHost = true,
            FailNext = method => method == "translate/languages"
                ? new InvalidOperationException("no translator installed")
                : null,
        };

        var log = new ListLogger();
        var (session, _, note) = TestSession.Create(engine: engine, log: log);

        Assert.Contains("stuck in the graphics driver", session.Status.LatestActivity);
        Assert.Contains(log.Lines, line => line.Contains("stray note host"));
        Assert.Empty(note.Languages);
        Assert.Contains(log.Lines, line => line.Contains("translate/languages failed"));
    }
}
