using System.Runtime.InteropServices;
using ClinicAVT.App.Platform;

namespace ClinicAVT.App.Tests.Metrics;

public class ProcessMetricsTest
{
    // An idle note host keeps its model after Windows trims its working set. Memory held but
    // never touched is the same case, so the chip must still count it
    [Fact]
    public void MemoryHeldButNotInUseStillCounts()
    {
        const long Held = 512L * 1024 * 1024;
        var metrics = new ProcessMetrics();
        var before = metrics.CommittedGb();
        var block = Marshal.AllocHGlobal((nint)Held);
        try
        {
            var after = metrics.CommittedGb();
            Assert.InRange(after - before, 0.45, 0.6);
        }
        finally
        {
            Marshal.FreeHGlobal(block);
        }
    }

    [Fact]
    public void AnImageThatIsNotRunningAddsNothing()
    {
        var metrics = new ProcessMetrics();
        var self = metrics.CommittedGb();
        Assert.True(self > 0);
        Assert.InRange(metrics.CommittedGb("clinicavt_no_such_process") - self, -0.05, 0.05);
    }
}
