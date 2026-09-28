using ClinicAVT.App.Core.Shell;

namespace ClinicAVT.App.Tests.Shell;

public class ThroughputMeterTest
{
    [Fact]
    public void TheWindowRollsWithTheStreamAStallDecaysToZeroAndEndFreezesTheValueUntilReset()
    {
        var meter = new ThroughputMeter(windowSeconds: 2.0);
        for (var i = 0; i < 20; i++)
        {
            meter.Token(i * 0.1);  // 10 tok/s for two seconds
        }

        Assert.True(meter.Streaming);
        Assert.Equal(10.0, meter.TokensPerSecond(2.0), 0);

        for (var i = 0; i <= 50; i++)
        {
            meter.Token(2.0 + i * 0.02);  // then 50 tok/s for one second
        }

        // Once the fast phase fills the window, the slow tokens have aged out
        Assert.True(meter.TokensPerSecond(3.1) > 25, "the rate follows the stream");

        // With nothing more arriving the window empties
        Assert.Equal(0, meter.TokensPerSecond(8.0));
        Assert.True(meter.Streaming, "stalled is not finished");

        meter = new ThroughputMeter();
        meter.Token(0.0);
        Assert.Equal(0, meter.TokensPerSecond(0.5));  // one token is not a rate

        for (var i = 1; i <= 10; i++)
        {
            meter.Token(i * 0.1);  // 10 tok/s
        }

        meter.End(1.0);
        Assert.False(meter.Streaming);
        Assert.Equal(10.0, meter.TokensPerSecond(60.0), 1);  // holds, hours later

        for (var i = 0; i <= 10; i++)
        {
            meter.Token(10.0 + i * 0.02);  // 50 tok/s regeneration
        }

        Assert.True(meter.Streaming);
        Assert.Equal(50.0, meter.TokensPerSecond(10.2), 0);

        meter.Reset();
        Assert.Equal(0, meter.TokensPerSecond(60.0));

        var idle = new ThroughputMeter();
        idle.End(1.0);
        Assert.Equal(0, idle.TokensPerSecond(2.0));
        Assert.False(idle.Streaming);
    }
}
