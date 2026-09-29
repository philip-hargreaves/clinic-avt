using System.Runtime.InteropServices;

namespace ClinicAVT.App.Platform.Tests;

public class ProcessMetricsTest
{
    // An idle note host keeps its model after Windows trims its working set. Memory held but
    // never touched is the same case, so the chip must still count it
    [Fact]
    public void MemoryHeldButNotInUseStillCountsAndAnImageThatIsNotRunningAddsNothing()
    {
        const long Held = 512L * 1024 * 1024;
        var metrics = new ProcessMetrics();
        var before = metrics.CommittedGb();
        Assert.True(before > 0);
        Assert.InRange(metrics.CommittedGb("clinicavt_no_such_process") - before, -0.05, 0.05);

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
}
